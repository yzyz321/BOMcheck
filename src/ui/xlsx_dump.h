#ifndef BS_XLSX_DUMP_H
#define BS_XLSX_DUMP_H

namespace bs {

/* --xlsx-dump <文件>：把工作簿结构逐格打到 stdout，供 tools/check_xlsx.py
   与 Python oracle 逐格对账。格式刻意做成一行一条、值转义，方便 diff。 */
int dump_xlsx(const char *path_utf8, void (*emit)(const char *line));

}  // namespace bs

#endif  // BS_XLSX_DUMP_H
