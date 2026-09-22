/*
 * DeepAgent 私有 quickjs 构建：不使用 NeoGraph 的符号前缀。
 *
 * NeoGraph 静态库内的 quickjs 经 deps/quickjs/neograph/quickjs-prefix.h
 * 重命名为 neograph_qjs_*；本工程把 NeoGraph/build/generated/quickjs-msvc
 * 的同一份适配源码以"无前缀"方式编入（此目录在 include 顺序上先于
 * deps/quickjs/neograph，故该空 shim 覆盖真实前缀头）。
 * 两份符号名不相交，可安全共存于同一可执行文件。
 */
#ifndef DEEPAGENT_QJS_EMPTY_PREFIX_H
#define DEEPAGENT_QJS_EMPTY_PREFIX_H
#endif
