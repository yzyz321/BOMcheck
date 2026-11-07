#ifndef BS_UI_BENCH_H
#define BS_UI_BENCH_H

#include <string>

namespace bs {

/* 纯计算基准：不建窗口、不碰 OLE，只走 core 的 读取 → 配对 → 12 列 → 写出。
   存在的理由是把「慢在哪一段」变成可测的数字，而不是靠界面日志里的一个总时长猜。
   返回 0 = 全流程成功。 */
int run_bench(const std::string &old_path, const std::string &new_path,
              const std::string &out_dir, bool parallel, void (*emit)(const char *line));

}  // namespace bs

#endif
