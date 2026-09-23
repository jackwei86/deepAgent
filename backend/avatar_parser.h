#pragma once

#include <string>
#include <vector>

namespace deepagent {

// .avatar（XML）解析器：提取 AvatarProject→timeline→project→nodes→node 下
// <param key="text_in"><primary><standard><track> 内的字幕文本。
// 手写轻量扫描（零第三方依赖），XML 实体反转义；node.ptr 为唯一主键。
struct AvatarTextEntry {
    int index = 0;           // 0 起序号（text_in 出现顺序）
    std::string nodeId;      // <node id="...">（块类型，如 org.uranus.block.voice_text）
    std::string nodePtr;     // <node ptr="...">（全局唯一）
    std::string text;        // 字幕文本（反转义后；单条上限 32KB）
};

// 解析 .avatar 文件；失败（不存在/非 AvatarProject XML/零条 text_in）返回 false 并填 error。
bool parseAvatarFile(const std::string& path, std::vector<AvatarTextEntry>* out,
                     std::string* error);

// XML 实体反转义（&amp; &lt; &gt; &quot; &apos; &#NN;）——独立导出供测试
std::string xmlUnescape(const std::string& in);

} // namespace deepagent
