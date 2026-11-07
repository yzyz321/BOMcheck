#ifndef BS_XML_H
#define BS_XML_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 极简 XML 解析器（回调式）。够用就行，不做 DOM：
   xlsx 的部件（sheetData 尤其）动辄几万节点，建树纯属浪费。
   不支持：DTD 实体展开、命名空间解析（调用方按前缀原样比对）、UTF-16。

   回调里的 name / 属性名 / 属性值只在回调期间有效，要留就自己拷。 */
typedef struct {
    void *ud;
    void (*on_open)(void *ud, const char *name, const char *const *attr, int nattr);
    void (*on_close)(void *ud, const char *name);
    void (*on_text)(void *ud, const char *s, size_t n);
} bsx_cb;

/* 成功返回 0。name / 值的对比用 strcmp，属性列表是 [名, 值, 名, 值, ...] */
int bsx_parse(const unsigned char *p, size_t n, const bsx_cb *cb, char *err, size_t errcap);

#ifdef __cplusplus
}
#endif

#endif /* BS_XML_H */
