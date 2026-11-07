/* core 的公开版本接口：不依赖 Win32，可单独单测 */
#ifndef BS_VERSION_H
#define BS_VERSION_H

#ifdef __cplusplus
extern "C" {
#endif

/* "4.0.1" */
const char *bs_version(void);

/* 编译时间戳，形如 "Nov  3 2026 22:50:11"，写进诊断报告用 */
const char *bs_build_stamp(void);

#ifdef __cplusplus
}
#endif

#endif /* BS_VERSION_H */
