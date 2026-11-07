#ifndef BS_UTF8_H
#define BS_UTF8_H

#include <string>

namespace bs {

/* 统一 UTF-8 ↔ UTF-16 转换。core 层全是 UTF-8（zip 里的部件名、xml 文本），
   Win32 一侧全是宽字符，边界就在这里。 */
std::string to_utf8(const std::wstring &s);
std::wstring from_utf8(const char *s);
/* 定长版：截断时保证有终止符 */
void from_utf8(const char *src, wchar_t *dst, int cap);

}  // namespace bs

#endif  // BS_UTF8_H
