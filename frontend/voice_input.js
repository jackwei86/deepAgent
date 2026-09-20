// voice_input.js — 语音输入（Web Speech API, zh-CN）
// 浏览器原生语音识别，无需后端 STT 服务。Chrome/Edge 支持；
// 不支持的环境下 onUnsupported 回调将麦克风按钮置灰。
(function (global) {
    'use strict';

    var SR = global.SpeechRecognition || global.webkitSpeechRecognition;

    function VoiceInput(options) {
        options = options || {};
        this.lang = options.lang || 'zh-CN';
        this.onInterim = options.onInterim || function () {};
        this.onFinal = options.onFinal || function () {};
        this.onStateChange = options.onStateChange || function () {};
        this.onError = options.onError || function () {};
        this.supported = !!SR;
        this.recognition = null;
        this.recording = false;
        this._finalText = '';
    }

    VoiceInput.prototype.start = function () {
        if (!this.supported || this.recording) return false;
        var self = this;
        var rec = new SR();
        rec.lang = this.lang;
        rec.continuous = true;
        rec.interimResults = true;

        this._finalText = '';
        rec.onresult = function (event) {
            var interim = '';
            for (var i = event.resultIndex; i < event.results.length; i++) {
                var r = event.results[i];
                if (r.isFinal) {
                    self._finalText += r[0].transcript;
                    self.onFinal(self._finalText);
                } else {
                    interim += r[0].transcript;
                }
            }
            if (interim) self.onInterim(self._finalText + interim);
        };
        rec.onend = function () {
            // auto-restart while the user is still recording (recognition
            // ends itself after a pause of speech)
            if (self.recording) {
                try { rec.start(); return; } catch (e) { /* fall through */ }
            }
            self.recording = false;
            self.onStateChange(false);
        };
        rec.onerror = function (event) {
            var code = event && event.error ? event.error : 'unknown';
            if (code === 'no-speech' || code === 'aborted') return;
            self.recording = false;
            self.onStateChange(false);
            self.onError(code);
        };

        this.recognition = rec;
        this.recording = true;
        try {
            rec.start();
        } catch (e) {
            this.recording = false;
            this.onError('start-failed');
            return false;
        }
        this.onStateChange(true);
        return true;
    };

    VoiceInput.prototype.stop = function () {
        if (!this.recording) return;
        this.recording = false;
        try {
            if (this.recognition) this.recognition.stop();
        } catch (e) { /* ignore */ }
        this.onStateChange(false);
    };

    VoiceInput.prototype.toggle = function () {
        return this.recording ? (this.stop(), false) : (this.start(), true);
    };

    global.VoiceInput = VoiceInput;
})(window);
