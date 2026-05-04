import argparse
import os
import re
import sys  # 引入 sys 模块用于终止程序
from pathlib import Path


def scan_and_generate(scan_dir, output_file, json_include_path):
    scan_path = Path(scan_dir)

    struct_pattern = re.compile(
        r"//\s*@JSON_ENABLE\s*struct\s+(\w+)[^{]*\{([\s\S]*?)\};"
    )
    var_pattern = re.compile(
        r"\b(?:[a-zA-Z_]\w*(?:::[a-zA-Z_]\w*)*(?:<[^>]+>)?)\s+([a-zA-Z_]\w*)[^;]*;"
    )

    generated_macros = []
    included_files = set()

    # 定义哪些后缀属于“源文件”
    source_extensions = {".cpp", ".cc", ".cxx", ".c"}

    # 注意：这里我们扫描 *.[hc]*，为了把 .cpp 也扫进来抓现行！
    for file_path in scan_path.rglob("*.[hc]*"):
        try:
            with open(file_path, "r", encoding="utf-8") as f:
                content = f.read()
        except:
            continue

        # 如果文件里没有标记，直接跳过
        if "// @JSON_ENABLE" not in content:
            continue

        # 🚨 【核心报错逻辑】检查到源文件里有宏，直接拦截并报错！
        if file_path.suffix.lower() in source_extensions:
            print("\n" + "=" * 60, file=sys.stderr)
            print(
                f"❌ [MyFramework 编译错误] 发现非法的自动 JSON 注册！", file=sys.stderr
            )
            print(f"📄 错误文件: {file_path}", file=sys.stderr)
            print(
                f"💡 原因: 带有 // @JSON_ENABLE 的结构体必须定义在头文件 (.h/.hpp) 中。",
                file=sys.stderr,
            )
            print(
                f"🛠️ 解决: 请将该结构体移至 .hpp 头文件内，否则会引发多重定义错误。",
                file=sys.stderr,
            )
            print("=" * 60 + "\n", file=sys.stderr)

            # 退出码 1 会告诉 CMake 脚本执行失败，立刻中断 C++ 编译！
            sys.exit(1)

        # 如果是合法的头文件，继续提取宏
        file_has_macro = False
        for match in struct_pattern.finditer(content):
            struct_name = match.group(1)
            vars = var_pattern.findall(match.group(2))
            if vars:
                args = ", ".join(vars)
                generated_macros.append(
                    f"NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE({struct_name}, {args})"
                )
                file_has_macro = True

        if file_has_macro:
            included_files.add(file_path.resolve().as_posix())

    # 生成全局头文件 (代码和以前一样)
    os.makedirs(os.path.dirname(output_file), exist_ok=True)
    with open(output_file, "w", encoding="utf-8") as f:
        f.write("// 本文件由框架自动生成\n")
        f.write("#pragma once\n")
        f.write(f"#include {json_include_path}\n\n")

        for inc in sorted(included_files):
            f.write(f'#include "{inc}"\n')

        f.write("\n// 宏展开区\n")
        for macro in generated_macros:
            f.write(macro + "\n")

    print(
        f"✅ JSON宏生成成功！共在 {len(included_files)} 个文件中找到了 {len(generated_macros)} 个结构体。"
    )


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("-d", "--dir", required=True)
    parser.add_argument("-o", "--output", required=True)
    parser.add_argument("-j", "--json-include", default="<nlohmann/json.hpp>")
    args = parser.parse_args()

    scan_and_generate(args.dir, args.output, args.json_include)
