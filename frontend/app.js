// app.js — DeepAgent 前端主逻辑：会话管理、SSE 流式渲染、udrt 结果卡片、语音输入
(function () {
    'use strict';

    // ---------- DOM ----------
    var $ = function (id) { return document.getElementById(id); };
    var chatListEl = $('chatList'), chatAreaEl = $('chatArea'), messagesEl = $('messages');
    var welcomeEl = $('welcome'), inputEl = $('inputText'), sendBtn = $('sendBtn');
    var micBtn = $('micBtn'), newChatBtn = $('newChatBtn'), kbBtn = $('kbBtn');
    var kbModal = $('kbModal'), kbBody = $('kbBody'), kbCloseBtn = $('kbCloseBtn');
    var healthDot = $('healthDot'), healthText = $('healthText'), modelSub = $('modelSub');

    var currentChatId = null;
    var sending = false;

    // ---------- Markdown 渲染（轻量实现，先转义防注入） ----------
    function escapeHtml(s) {
        return s.replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;')
            .replace(/"/g, '&quot;');
    }

    function renderMarkdown(text) {
        var blocks = [];
        // 提取代码块
        text = text.replace(/```(\w*)\n?([\s\S]*?)```/g, function (_, lang, code) {
            blocks.push('<pre><code>' + escapeHtml(code.replace(/\n$/, '')) + '</code></pre>');
            return '\u0000B' + (blocks.length - 1) + '\u0000';
        });
        var esc = escapeHtml(text);
        esc = esc
            .replace(/^### (.+)$/gm, '<h3>$1</h3>')
            .replace(/^## (.+)$/gm, '<h3>$1</h3>')
            .replace(/^# (.+)$/gm, '<h3>$1</h3>')
            .replace(/\*\*([^*\n]+)\*\*/g, '<b>$1</b>')
            .replace(/(^|[^*])\*([^*\n]+)\*(?!\*)/g, '$1<i>$2</i>')
            .replace(/`([^`\n]+)`/g, '<code>$1</code>')
            .replace(/^\s*[-*] (.+)$/gm, '<li>$1</li>')
            .replace(/^\s*\d+\. (.+)$/gm, '<li>$1</li>');
        esc = esc.replace(/(<li>[\s\S]*?<\/li>)(?!\s*<li>)/g, '<ul>$1</ul>');
        // 段落与换行
        var parts = esc.split(/\n{2,}/);
        for (var i = 0; i < parts.length; i++) {
            var p = parts[i];
            if (!p.trim()) continue;
            if (/^<(h3|ul|pre|\u0000)/.test(p.trim())) continue;
            parts[i] = '<p>' + p.replace(/\n/g, '<br>') + '</p>';
        }
        var html = parts.join('');
        html = html.replace(/\u0000B(\d+)\u0000/g, function (_, idx) {
            return blocks[+idx];
        });
        return html;
    }

    // ---------- 健康检查 / 知识库 ----------
    function checkHealth() {
        fetch('/api/health').then(function (r) { return r.json(); }).then(function (j) {
            var ok = j.status === 'ok';
            healthDot.className = 'dot ' + (ok ? 'ok' : 'bad');
            healthText.textContent = (ok ? '后端正常 · ' : '后端异常 · ') +
                (j.llm_configured ? j.llm_model : 'LLM 未配置');
            modelSub.textContent = 'Graph_Saturn 工具 Node 编排助手 · ' +
                (j.knowledge_entries || 0) + ' 条知识库条目';
        }).catch(function () {
            healthDot.className = 'dot bad';
            healthText.textContent = '后端连接失败';
        });
    }

    function openKnowledge() {
        kbModal.style.display = 'flex';
        kbBody.textContent = '加载中…';
        fetch('/api/knowledge').then(function (r) { return r.json(); }).then(function (j) {
            kbBody.innerHTML = '';
            (j.entries || []).forEach(function (e) {
                var div = document.createElement('div');
                div.className = 'kb-entry';
                var tag = e.kind === 'real' ? '<span class="kb-tag real">真实节点</span>'
                    : '<span class="kb-tag">预设节点</span>';
                div.innerHTML =
                    '<div class="ke-head"><span class="ke-name">' + escapeHtml(e.name || '') + '</span>' +
                    '<code>' + escapeHtml(e.module_id || '') + '</code>' + tag + '</div>' +
                    '<div class="ke-line"><b>条目 id：</b><code>' + escapeHtml(e.id || '') + '</code>' +
                    ' · <b>项目：</b>' + escapeHtml(e.project || '') + '</div>' +
                    '<div class="ke-line"><b>能力：</b>' + escapeHtml(e.description || '') + '</div>' +
                    '<div class="ke-line"><b>输入：</b>' + escapeHtml(e.input || '') + '</div>' +
                    '<div class="ke-line"><b>输出：</b>' + escapeHtml(e.output || '') + '</div>';
                kbBody.appendChild(div);
            });
            if (!kbBody.children.length) kbBody.textContent = '知识库为空';
        }).catch(function () { kbBody.textContent = '加载失败'; });
    }

    // ---------- 会话列表 ----------
    var archivedOpen = false;   // 归档分组展开状态（跨重渲染保持）

    function loadChatList(selectId) {
        fetch('/api/chats').then(function (r) { return r.json(); }).then(function (chats) {
            chatListEl.innerHTML = '';
            var all = chats || [];
            var active = all.filter(function (c) { return !c.archived; });
            var archived = all.filter(function (c) { return c.archived; })
                .sort(function (a, b) { return (b.archivedAtMs || 0) - (a.archivedAtMs || 0); });

            active.forEach(function (c) {
                chatListEl.appendChild(makeChatItem(c, false));
            });

            // 已归档分区（可折叠分组：右侧箭头指示折叠状态）
            if (archived.length) {
                var header = document.createElement('div');
                header.className = 'archived-header' + (archivedOpen ? ' open' : '');
                header.innerHTML = '<span class="archived-title">已归档</span>' +
                    '<span class="chevron">▾</span>' +
                    '<span class="count">' + archived.length + '</span>';
                var group = document.createElement('div');
                group.className = 'archived-group' + (archivedOpen ? '' : ' collapsed');
                header.onclick = function () {
                    archivedOpen = !archivedOpen;
                    header.classList.toggle('open', archivedOpen);
                    group.classList.toggle('collapsed', !archivedOpen);
                };
                chatListEl.appendChild(header);
                chatListEl.appendChild(group);
                archived.forEach(function (c) {
                    group.appendChild(makeChatItem(c, true));
                });
            }

            if (selectId) selectChat(selectId);
        }).catch(function () {});
    }

    function makeChatItem(c, archived) {
        var item = document.createElement('div');
        item.className = 'chat-item' + (c.id === currentChatId ? ' active' : '');
        item.innerHTML = '<span class="chat-icon">' + (archived ? '🗄' : '💬') + '</span>' +
            '<span class="chat-title">' + escapeHtml(c.title || '新会话') + '</span>' +
            (archived
                ? '<span class="chat-del" title="删除会话（不可恢复）">🗑</span>'
                : '<span class="chat-arc" title="归档会话">📥</span>');
        item.onclick = function () { selectChat(c.id); };
        var btn = item.querySelector(archived ? '.chat-del' : '.chat-arc');
        btn.onclick = function (e) {
            e.stopPropagation();
            archived ? deleteChat(c.id) : archiveChat(c.id);
        };
        return item;
    }

    function archiveChat(id) {
        if (!confirm('归档该会话？\n归档不删除任何数据：会话移入"已归档"列表，随时可查看或继续对话。')) return;
        fetch('/api/chats/' + id + '/archive', { method: 'POST' }).then(function (r) {
            if (!r.ok) return;
            if (id === currentChatId) {
                // 归档当前会话：保留查看状态，仅刷新列表归位
                loadChatList();
            } else {
                loadChatList();
            }
        }).catch(function () {});
    }

    function deleteChat(id) {
        if (!confirm('确定删除这个已归档的会话吗？删除后不可恢复。')) return;
        fetch('/api/chats/' + id, { method: 'DELETE' }).then(function (r) {
            if (!r.ok) return;
            if (id === currentChatId) {
                newChat();          // 删除的是当前会话：回到新会话欢迎页
            } else {
                loadChatList();
            }
        }).catch(function () {});
    }

    function selectChat(id) {
        currentChatId = id;
        fetch('/api/chats/' + id).then(function (r) { return r.json(); }).then(function (chat) {
            messagesEl.innerHTML = '';
            welcomeEl.style.display = 'none';
            (chat.messages || []).forEach(function (m) {
                appendMessage(m.role, m.content, m.role === 'assistant');
            });
            scrollBottom();
            loadChatList();
        }).catch(function () {});
    }

    function newChat() {
        currentChatId = null;
        messagesEl.innerHTML = '';
        welcomeEl.style.display = 'block';
        loadChatList();
        inputEl.focus();
    }

    // ---------- 消息渲染 ----------
    function appendMessage(role, content, finalize) {
        var msg = document.createElement('div');
        msg.className = 'msg ' + (role === 'user' ? 'user' : 'assistant');
        var avatar = document.createElement('div');
        avatar.className = 'avatar';
        avatar.textContent = role === 'user' ? '🧑' : '🤖';
        var bubble = document.createElement('div');
        bubble.className = 'bubble';
        var contentEl = document.createElement('div');
        contentEl.className = 'content';
        if (role === 'user') {
            contentEl.textContent = content;
        } else {
            contentEl.innerHTML = renderMarkdown(content);
        }
        // 原始文本（assistant 流式回复期间由 token/done 处理器持续更新）
        bubble.__raw = content;

        // 复制按钮：复制这条消息的全文（用户消息 / 助手回复通用）
        var copyBtn = document.createElement('button');
        copyBtn.className = 'copy-btn';
        copyBtn.type = 'button';
        copyBtn.title = '复制全文';
        copyBtn.textContent = '复制';
        copyBtn.addEventListener('click', function () {
            var text = (bubble.__raw !== undefined && bubble.__raw !== null)
                ? String(bubble.__raw)
                : contentEl.innerText;
            copyText(text, copyBtn);
        });

        bubble.appendChild(contentEl);
        bubble.appendChild(copyBtn);
        msg.appendChild(avatar);
        msg.appendChild(bubble);
        messagesEl.appendChild(msg);
        scrollBottom();
        return { msg: msg, bubble: bubble, content: contentEl };
    }

    // 复制到剪贴板（优先异步 clipboard API，http://127.0.0.1 属安全上下文；
    // execCommand 兜底），按钮给出 1.2s "已复制" 反馈
    function copyText(text, btn) {
        function feedback() {
            btn.textContent = '已复制 ✓';
            btn.classList.add('copied');
            setTimeout(function () {
                btn.textContent = '复制';
                btn.classList.remove('copied');
            }, 1200);
        }
        function legacyCopy() {
            var ta = document.createElement('textarea');
            ta.value = text;
            ta.style.position = 'fixed';
            ta.style.opacity = '0';
            document.body.appendChild(ta);
            ta.select();
            try { document.execCommand('copy'); } catch (e) { /* ignore */ }
            document.body.removeChild(ta);
        }
        if (navigator.clipboard && navigator.clipboard.writeText) {
            navigator.clipboard.writeText(text).then(feedback, function () { legacyCopy(); feedback(); });
        } else {
            legacyCopy();
            feedback();
        }
    }

    function scrollBottom() {
        chatAreaEl.scrollTop = chatAreaEl.scrollHeight;
    }

    function appendStatusStream(bubble) {
        // 状态条 + 动态内容区的容器（放在 assistant 气泡内，content 之前）
        var chip = document.createElement('div');
        chip.className = 'status-chip';
        chip.innerHTML = '<span class="spin"></span><span class="text">准备中…</span>';
        bubble.insertBefore(chip, bubble.firstChild);
        return {
            set: function (text) {
                chip.querySelector('.text').textContent = text;
                chip.classList.remove('done');
                chip.innerHTML = '<span class="spin"></span><span class="text"></span>';
                chip.querySelector('.text').textContent = text;
                scrollBottom();
            },
            done: function () {
                chip.classList.add('done');
                chip.innerHTML = '<span class="text"></span>';
            }
        };
    }

    // HTML 资产卡片（对话中直接生成的资产，与 udrt 同等资产管理）
    function appendAssetCard(bubble, payload) {
        var card = document.createElement('div');
        card.className = 'udrt-card asset-card';
        var fileName = (payload.file || '').split(/[\/]/).pop();
        var previewSrc = '/api/asset/content?path=' + encodeURIComponent(payload.file || '') +
                         '&t=' + Date.now();
        card.innerHTML =
            '<div class="uc-head"><span class="uc-title">🌐 已生成 HTML 资产文件</span></div>' +
            '<div class="uc-meta">关联节点 GUID：<code>' + escapeHtml(payload.guid || '') + '</code>' +
            ' · 版本：v' + (payload.version || 1) + '<br>' +
            '文件：' + escapeHtml(payload.absolute_path || payload.file || '') + '</div>' +
            '<div class="asset-preview"><iframe sandbox="" src="' + previewSrc + '" ' +
            'loading="lazy" title="资产预览"></iframe></div>' +
            '<div class="uc-actions">' +
            '<button class="view">🌐 在浏览器查看</button>' +
            '<button class="open-dir">📂 打开所在目录</button>' +
            '<button class="dl">⬇ 下载 .html</button>' +
            '<button class="optimize">✨ 优化</button>' +
            '<button class="toggle-preview">⤴ 收起预览</button>' +
            '</div>';
        card.querySelector('.view').onclick = function () {
            window.open('/api/asset/content?path=' + encodeURIComponent(payload.file || ''), '_blank');
        };
        card.querySelector('.open-dir').onclick = function () {
            fetch('/api/open_folder', {
                method: 'POST',
                headers: { 'Content-Type': 'application/json' },
                body: JSON.stringify({ path: payload.file || '' })
            }).then(function (r) { return r.json(); }).then(function (j) {
                if (!j.opened) alert('打开目录失败：' + (j.error || '未知错误'));
            }).catch(function (e) { alert('打开目录失败：' + e); });
        };
        card.querySelector('.dl').onclick = function () {
            fetch('/api/asset/content?path=' + encodeURIComponent(payload.file || ''))
                .then(function (r) { return r.text(); }).then(function (t) {
                    var blob = new Blob([t], { type: 'text/html' });
                    var a = document.createElement('a');
                    a.href = URL.createObjectURL(blob);
                    a.download = fileName || 'asset.html';
                    a.click();
                    URL.revokeObjectURL(a.href);
                });
        };
        card.querySelector('.optimize').onclick = function () {
            if (sending) { alert('当前有消息处理中，请稍后再试'); return; }
            inputEl.value = '请质检并优化这个 HTML 资产：' + (payload.file || '') +
                (payload.guid ? '（guid: ' + payload.guid + '）' : '') +
                '。请先读取内容评估质量，有问题直接优化并覆盖原文件，质量良好则简要说明评估结论。';
            sendMessage();
        };
        card.querySelector('.toggle-preview').onclick = function () {
            var box = card.querySelector('.asset-preview');
            var hidden = box.classList.toggle('collapsed');
            this.textContent = hidden ? '⤵ 展开预览' : '⤴ 收起预览';
        };
        bubble.insertBefore(card, bubble.querySelector('.content'));
        scrollBottom();
    }

    function appendUdrtCard(bubble, payload) {
        var card = document.createElement('div');
        card.className = 'udrt-card';
        var fileName = (payload.file || '').split(/[\\\/]/).pop();
        card.innerHTML =
            '<div class="uc-head"><span class="uc-title">📄 已生成 udrt 工程文件</span></div>' +
            '<div class="uc-meta">工具 Node：<b>' + escapeHtml(payload.name || '') + '</b>' +
            '（<code>' + escapeHtml(payload.module_id || '') + '</code>）<br>' +
            '节点数：' + payload.node_count + ' · 连接数：' + payload.connection_count + '<br>' +
            '文件：' + escapeHtml(payload.absolute_path || payload.file || '') + '</div>' +
            '<div class="uc-actions">' +
            '<button class="dl">⬇ 下载 .udrt</button>' +
            '<button class="open-dir">📂 打开所在目录</button>' +
            '<details><summary>查看 udrt JSON</summary><pre></pre></details>' +
            '</div>';
        card.querySelector('pre').textContent = JSON.stringify(payload.content || {}, null, 4);
        card.querySelector('.dl').onclick = function () {
            var blob = new Blob([JSON.stringify(payload.content || {}, null, 4)],
                { type: 'application/json' });
            var a = document.createElement('a');
            a.href = URL.createObjectURL(blob);
            a.download = fileName || 'graph.udrt';
            a.click();
            URL.revokeObjectURL(a.href);
        };
        card.querySelector('.open-dir').onclick = function () {
            fetch('/api/open_folder', {
                method: 'POST',
                headers: { 'Content-Type': 'application/json' },
                body: JSON.stringify({ path: payload.file || '' })
            }).then(function (r) { return r.json(); }).then(function (j) {
                if (!j.opened) alert('打开目录失败：' + (j.error || '未知错误'));
            }).catch(function (e) { alert('打开目录失败：' + e); });
        };
        bubble.insertBefore(card, bubble.querySelector('.content'));
        scrollBottom();
    }

    function appendError(bubble, message) {
        var chip = document.createElement('div');
        chip.className = 'error-chip';
        chip.textContent = '出错了：' + message;
        bubble.insertBefore(chip, bubble.querySelector('.content'));
        scrollBottom();
    }

    // ---------- 附件管理 ----------
    var pendingAttachments = [];   // [{filename, content}]

    var fileInputEl = document.createElement('input');
    fileInputEl.type = 'file';
    fileInputEl.accept = '.udrt,.xml,.json,.txt,.md';
    fileInputEl.multiple = true;
    fileInputEl.style.display = 'none';
    document.body.appendChild(fileInputEl);

    fileInputEl.addEventListener('change', function () {
        var files = fileInputEl.files;
        if (!files.length) return;
        var loaded = 0;
        Array.prototype.forEach.call(files, function (f) {
            var reader = new FileReader();
            reader.onload = function () {
                pendingAttachments.push({ filename: f.name, content: reader.result });
                renderAttachmentTags();
                if (++loaded === files.length) fileInputEl.value = '';
            };
            reader.readAsText(f);
        });
    });

    function renderAttachmentTags() {
        var box = document.getElementById('attachmentTags');
        if (!box) return;
        box.innerHTML = '';
        pendingAttachments.forEach(function (a, i) {
            var tag = document.createElement('span');
            tag.className = 'attach-tag';
            tag.innerHTML = '📎 ' + escapeHtml(a.filename) +
                '<span class="tag-del" data-i="' + i + '">✕</span>';
            tag.querySelector('.tag-del').onclick = function () {
                pendingAttachments.splice(i, 1);
                renderAttachmentTags();
            };
            box.appendChild(tag);
        });
        box.style.display = pendingAttachments.length ? 'flex' : 'none';
    }

    function clearAttachments() {
        pendingAttachments = [];
        renderAttachmentTags();
    }

    document.getElementById('attachBtn').onclick = function () { fileInputEl.click(); };

    // ---------- 发送消息（SSE 流式） ----------
    function sendMessage() {
        var text = inputEl.value.trim();
        if (!text || sending) return;
        var attachments = pendingAttachments.splice(0);   // 取走并清空
        sending = true;
        sendBtn.disabled = true;
        inputEl.value = '';
        renderAttachmentTags();
        autoGrow();

        welcomeEl.style.display = 'none';
        appendMessage('user', text, true);

        var holder = appendMessage('assistant', '', false);
        var contentEl = holder.content;
        var status = appendStatusStream(holder.bubble);
        var acc = '';

        var bodyPayload = { chat_id: currentChatId, message: text };
        if (attachments.length) bodyPayload.attachments = attachments;

        fetch('/api/chat/stream', {
            method: 'POST',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify(bodyPayload)
        }).then(function (res) {
            if (!res.ok) {
                // 非 2xx：读取错误信息并以 error 事件呈现，避免静默无回复
                return res.text().then(function (t) {
                    var msg = 'HTTP ' + res.status;
                    try { msg = JSON.parse(t).error || msg; } catch (e) { /* keep */ }
                    status.done();
                    appendError(holder.bubble, msg);
                    sending = false;
                    sendBtn.disabled = false;
                });
            }
            var headerChatId = res.headers.get('X-Chat-Id');
            if (headerChatId && !currentChatId) currentChatId = headerChatId;
            var reader = res.body.getReader();
            var decoder = new TextDecoder('utf-8');
            var buf = '';

            function pump() {
                return reader.read().then(function (r) {
                    if (r.done) { finish(); return; }
                    buf += decoder.decode(r.value, { stream: true });
                    var idx;
                    while ((idx = buf.indexOf('\n\n')) >= 0) {
                        var frame = buf.slice(0, idx);
                        buf = buf.slice(idx + 2);
                        handleFrame(frame);
                    }
                    return pump();
                });
            }

            function handleFrame(frame) {
                var event = 'message', data = '';
                frame.split('\n').forEach(function (line) {
                    if (line.indexOf('event:') === 0) event = line.slice(6).trim();
                    else if (line.indexOf('data:') === 0) data += line.slice(5).trim();
                });
                if (!data) return;
                var j;
                try { j = JSON.parse(data); } catch (e) { return; }
                if (event === 'status') {
                    status.set(j.description || '');
                } else if (event === 'token') {
                    status.done();
                    acc += j.delta || '';
                    holder.bubble.__raw = acc;
                    contentEl.innerHTML = renderMarkdown(acc);
                    scrollBottom();
                } else if (event === 'udrt') {
                    appendUdrtCard(holder.bubble, j);
                } else if (event === 'asset') {
                    appendAssetCard(holder.bubble, j);
                } else if (event === 'error') {
                    status.done();
                    appendError(holder.bubble, j.message || '未知错误');
                } else if (event === 'done') {
                    finishFrame = j;
                }
            }

            var finishFrame = null;
            function finish() {
                if (!acc && finishFrame && finishFrame.reply) {
                    holder.bubble.__raw = finishFrame.reply;
                    contentEl.innerHTML = renderMarkdown(finishFrame.reply);
                }
                if (!acc && !finishFrame) {
                    contentEl.innerHTML = '<i style="color:#9ca3af">（无回复）</i>';
                }
                sending = false;
                sendBtn.disabled = false;
                loadChatList();
                inputEl.focus();
            }

            return pump();
        }).catch(function (e) {
            status.done();
            appendError(holder.bubble, String(e));
            sending = false;
            sendBtn.disabled = false;
        });
    }

    // ---------- 输入框 ----------
    function autoGrow() {
        inputEl.style.height = 'auto';
        inputEl.style.height = Math.min(inputEl.scrollHeight, 180) + 'px';
    }

    inputEl.addEventListener('input', autoGrow);
    inputEl.addEventListener('keydown', function (e) {
        if (e.key === 'Enter' && !e.shiftKey && !e.isComposing) {
            e.preventDefault();
            sendMessage();
        }
    });
    sendBtn.onclick = sendMessage;
    newChatBtn.onclick = newChat;
    kbBtn.onclick = openKnowledge;
    kbCloseBtn.onclick = function () { kbModal.style.display = 'none'; };
    kbModal.onclick = function (e) { if (e.target === kbModal) kbModal.style.display = 'none'; };

    // 侧栏折叠
    var sidebar = $('sidebar');
    $('collapseBtn').onclick = function () { sidebar.classList.toggle('collapsed'); };

    // 欢迎卡片
    Array.prototype.forEach.call(document.querySelectorAll('.card'), function (card) {
        card.onclick = function () {
            inputEl.value = card.getAttribute('data-prompt') || '';
            autoGrow();
            sendMessage();
        };
    });

    // ---------- 语音输入 ----------
    var voice = new VoiceInput({
        lang: 'zh-CN',
        onInterim: function (text) { inputEl.value = text; autoGrow(); },
        onFinal: function (text) { inputEl.value = text; autoGrow(); },
        onStateChange: function (recording) {
            micBtn.classList.toggle('recording', recording);
            micBtn.title = recording ? '停止语音输入' : '语音输入（点击开始）';
        },
        onError: function (code) {
            alert('语音识别出错：' + code + '\n请检查麦克风权限与网络连接。');
        }
    });
    if (!voice.supported) {
        micBtn.disabled = true;
        micBtn.title = '当前浏览器不支持语音识别（Web Speech API），请使用 Chrome/Edge';
    } else {
        micBtn.onclick = function () { voice.toggle(); };
    }

    // ---------- 启动 ----------
    checkHealth();
    loadChatList();
    newChat();
})();
