/* 自检模式：在运行时读自己的资源（manifest / 版本），不依赖外部 dump 工具。
   这样现场也能用 exe 自证「DPI 声明在不在、manifest 是不是只有一份」。 */
#ifndef BS_SELFCHECK_H
#define BS_SELFCHECK_H

namespace bs {

/* 打印 key=value 行；返回 0 全部达标，非 0 表示有项不达标 */
int run_self_check(void (*emit)(const char *line));

}  // namespace bs

#endif  // BS_SELFCHECK_H
