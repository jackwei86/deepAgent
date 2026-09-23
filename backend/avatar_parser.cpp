#include "avatar_parser.h"

#include <fstream>
#include <sstream>

namespace deepagent {
namespace {

constexpr size_t kMaxEntryBytes = 32 * 1024;

// 大小写不敏感的子串查找（标签名统一按小写匹配；attribute 值原样保留）
size_t findCi(const std::string& hay, const std::string& needle, size_t from) {
    if (needle.empty() || hay.size() < needle.size()) return std::string::npos;
    if (from > hay.size() - needle.size()) return std::string::npos;
    const size_t last = hay.size() - needle.size();
    for (size_t i = from; i <= last; ++i) {
        size_t j = 0;
        for (; j < needle.size(); ++j) {
            char a = hay[i + j], b = needle[j];
            if (a >= 'A' && a <= 'Z') a = static_cast<char>(a - 'A' + 'a');
            if (b >= 'A' && b <= 'Z') b = static_cast<char>(b - 'A' + 'a');
            if (a != b) break;
        }
        if (j == needle.size()) return i;
    }
    return std::string::npos;
}

// 提取 <tag ... attr="value" ...> 形式起始标签中某属性的值
std::string extractAttr(const std::string& openTag, const std::string& attr) {
    const std::string needle = attr + "=\"";
    size_t p = findCi(openTag, needle, 0);
    if (p == std::string::npos) return {};
    p += needle.size();
    const size_t end = openTag.find('"', p);
    if (end == std::string::npos) return {};
    return openTag.substr(p, end - p);
}

} // namespace

std::string xmlUnescape(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    for (size_t i = 0; i < in.size();) {
        if (in[i] != '&') {
            out += in[i++];
            continue;
        }
        const size_t semi = in.find(';', i);
        if (semi == std::string::npos || semi - i > 10) {
            out += in[i++];
            continue;
        }
        const std::string ent = in.substr(i + 1, semi - i - 1);
        if (ent == "amp") out += '&';
        else if (ent == "lt") out += '<';
        else if (ent == "gt") out += '>';
        else if (ent == "quot") out += '"';
        else if (ent == "apos") out += '\'';
        else if (!ent.empty() && ent[0] == '#') {
            // &#NN; 十进制 / &#xHH; 十六进制
            try {
                const int code = ent.size() > 1 && (ent[1] == 'x' || ent[1] == 'X')
                                     ? std::stoi(ent.substr(2), nullptr, 16)
                                     : std::stoi(ent.substr(1));
                if (code > 0 && code < 0x110000) {
                    // UTF-8 编码（BMP 内常用值）
                    unsigned int cp = static_cast<unsigned int>(code);
                    if (cp < 0x80) out += static_cast<char>(cp);
                    else if (cp < 0x800) {
                        out += static_cast<char>(0xC0 | (cp >> 6));
                        out += static_cast<char>(0x80 | (cp & 0x3F));
                    } else {
                        out += static_cast<char>(0xE0 | (cp >> 12));
                        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                        out += static_cast<char>(0x80 | (cp & 0x3F));
                    }
                }
            } catch (const std::exception&) {
                // 非法数字实体：原样保留
                out += in.substr(i, semi - i + 1);
            }
        } else {
            out += in.substr(i, semi - i + 1);  // 未知实体：原样保留
        }
        i = semi + 1;
    }
    return out;
}

bool parseAvatarFile(const std::string& path, std::vector<AvatarTextEntry>* out,
                     std::string* error) {
    if (out) out->clear();
    auto fail = [&](const std::string& msg) {
        if (error) *error = msg;
        if (out) out->clear();
        return false;
    };

    if (path.empty()) return fail("avatar path is empty");

    std::ifstream ifs(path, std::ios::binary);
    if (!ifs) return fail("cannot open avatar file: " + path);
    std::ostringstream ss;
    ss << ifs.rdbuf();
    std::string xml = ss.str();
    if (xml.empty()) return fail("avatar file is empty: " + path);
    if (findCi(xml, "<avatarproject", 0) == std::string::npos)
        return fail("not an AvatarProject XML (missing <AvatarProject>): " + path);

    // 扫描每个 <node ...> ... </node> 段，找 param key="text_in" 的 track 文本
    int index = 0;
    size_t pos = 0;
    while (true) {
        const size_t nodeBegin = findCi(xml, "<node ", pos);
        if (nodeBegin == std::string::npos) break;
        const size_t nodeOpenEnd = xml.find('>', nodeBegin);
        if (nodeOpenEnd == std::string::npos) break;
        const std::string openTag = xml.substr(nodeBegin, nodeOpenEnd - nodeBegin + 1);
        const size_t nodeEnd = findCi(xml, "</node>", nodeOpenEnd);
        const size_t segEnd = (nodeEnd == std::string::npos) ? xml.size() : nodeEnd;
        const std::string seg = xml.substr(nodeOpenEnd + 1, segEnd - nodeOpenEnd - 1);

        const size_t paramPos = findCi(seg, "<param key=\"text_in\">", 0);
        if (paramPos != std::string::npos) {
            AvatarTextEntry e;
            e.index = index++;
            e.nodeId = extractAttr(openTag, "id");
            e.nodePtr = extractAttr(openTag, "ptr");
            // track 文本：param 内第一个 <track>...</track>（跨行内容原样保留）
            size_t tBegin = findCi(seg, "<track>", paramPos);
            if (tBegin == std::string::npos) {
                // 自闭合/带属性变体兜底：<track ...>
                tBegin = findCi(seg, "<track ", paramPos);
            }
            if (tBegin != std::string::npos) {
                size_t textBegin = seg.find('>', tBegin);
                const size_t textEnd = findCi(seg, "</track>", tBegin);
                if (textBegin != std::string::npos && textEnd != std::string::npos &&
                    textEnd > textBegin) {
                    ++textBegin;
                    std::string raw = seg.substr(textBegin, textEnd - textBegin);
                    // 去首尾空白（保留内部换行）
                    size_t b = raw.find_first_not_of(" \t\r\n");
                    size_t epos = raw.find_last_not_of(" \t\r\n");
                    raw = (b == std::string::npos) ? std::string()
                                                   : raw.substr(b, epos - b + 1);
                    e.text = xmlUnescape(raw);
                }
            }
            if (e.text.empty()) {
                return fail("text_in param with empty/missing <track> at node ptr=" +
                            e.nodePtr + " in " + path);
            }
            if (e.text.size() > kMaxEntryBytes) e.text.resize(kMaxEntryBytes);
            if (out) out->push_back(std::move(e));
        }
        if (nodeEnd == std::string::npos) break;
        pos = nodeEnd + 7;
    }

    if (index == 0)
        return fail("no <param key=\"text_in\"> found in avatar: " + path);
    if (error) error->clear();
    return true;
}

} // namespace deepagent
