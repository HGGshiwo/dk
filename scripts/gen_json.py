import argparse
import os
import re
import sys
from pathlib import Path
from textwrap import dedent


def scan_and_generate(scan_dir, output_file, json_include_path, namespace):
    scan_path = Path(scan_dir)

    struct_pattern = re.compile(
        r"//\s*@JSON_ENABLE\s*struct\s+(\w+)[^{]*\{([\s\S]*?)\};"
    )
    var_pattern = re.compile(
        r"(?:[a-zA-Z_]\w*(?:::[a-zA-Z_]\w*)*(?:<[^;={}]+>)?)\s+([a-zA-Z_]\w*)\s*(?:=[^;]*)?;([^\n]*)"
    )
    enum_pattern = re.compile(
        r"//\s*@JSON_ENABLE\s*enum\s+(?:class\s+)?(\w+)[^{]*\{([\s\S]*?)\};"
    )
    alias_pattern = re.compile(r'//\s*@JSON\(\s*"([^"]+)"\s*\)')
    base_pattern = re.compile(r"//\s*@JSON_BASE\(\s*(\w+)\s*\)")

    generated_macros = []
    included_files = set()
    source_extensions = {".cpp", ".cc", ".cxx", ".c"}

    for file_path in scan_path.rglob("*.[hc]*"):
        try:
            with open(file_path, "r", encoding="utf-8") as f:
                content = f.read()
        except:
            continue

        if "// @JSON_ENABLE" not in content:
            continue

        if file_path.suffix.lower() in source_extensions:
            print(
                f"❌ [编译错误] 源文件 {file_path} 中发现自动注册标记！请移至头文件。",
                file=sys.stderr,
            )
            sys.exit(1)

        file_has_macro = False

        # 1. 解析 Struct
        for match in struct_pattern.finditer(content):
            struct_name = match.group(1)
            struct_body = match.group(2)
            file_has_macro = True

            to_json_lines = []
            from_json_lines = []

            for base_match in base_pattern.finditer(struct_body):
                base_name = base_match.group(1)
                to_json_lines.append(
                    f"    to_json(j, static_cast<const {base_name}&>(t));"
                )
                from_json_lines.append(
                    f"    from_json(j, static_cast<{base_name}&>(t));"
                )

            for var_match in var_pattern.finditer(struct_body):
                var_name = var_match.group(1)
                trailing_text = var_match.group(2)

                alias_match = alias_pattern.search(trailing_text)
                json_key = alias_match.group(1) if alias_match else var_name

                to_json_lines.append(f'    j["{json_key}"] = t.{var_name};')
                from_json_lines.append(
                    f'    if (j.contains("{json_key}") && !j.at("{json_key}").is_null()) j.at("{json_key}").get_to(t.{var_name});'
                )

            code = f"""
// Auto-generated for Struct: {struct_name}
inline void to_json(nlohmann::json& j, const {struct_name}& t) {{
{chr(10).join(to_json_lines)}
}}
inline void from_json(const nlohmann::json& j, {struct_name}& t) {{
{chr(10).join(from_json_lines)}
}}"""
            generated_macros.append(code.strip())

        # 2. 解析 Enum
        for match in enum_pattern.finditer(content):
            enum_name = match.group(1)
            enum_body = match.group(2)
            file_has_macro = True

            pairs = []
            for item in enum_body.split(","):
                item = item.strip()
                if not item:
                    continue
                val_match = re.match(r"^([a-zA-Z_]\w*)", item)
                if not val_match:
                    continue

                enum_item_name = val_match.group(1)
                alias_match = alias_pattern.search(item)
                json_string = alias_match.group(1) if alias_match else enum_item_name

                pairs.append(f'    {{{enum_name}::{enum_item_name}, "{json_string}"}}')

            if pairs:
                pairs_str = ",\n".join(pairs)
                macro = (
                    f"NLOHMANN_JSON_SERIALIZE_ENUM({enum_name}, {{\n{pairs_str}\n}})"
                )
                generated_macros.append(macro)

        if file_has_macro:
            included_files.add(file_path.resolve().as_posix())

    # 生成全局头文件
    os.makedirs(os.path.dirname(output_file), exist_ok=True)
    with open(output_file, "w", encoding="utf-8") as f:
        f.write("// 本文件由框架自动生成\n")
        f.write("#pragma once\n")
        f.write(f"#include {json_include_path}\n")
        f.write("#include <optional>\n\n")

        for inc in sorted(included_files):
            f.write(f'#include "{inc}"\n')

        # ==========================================
        # 🌟 核心修改处：自动注入 optional 序列化补丁
        # ==========================================
        f.write(
            """
// =====================================================================
// 自动注入: 支持 std::optional 的 JSON 序列化器
// =====================================================================
#ifndef AUTO_JSON_OPTIONAL_SERIALIZER_INJECTED
#define AUTO_JSON_OPTIONAL_SERIALIZER_INJECTED
namespace nlohmann {
    template <typename T>
    struct adl_serializer<std::optional<T>> {
        static void to_json(json& j, const std::optional<T>& opt) {
            if (opt.has_value()) {
                j = *opt;
            } else {
                j = nullptr;
            }
        }
        static void from_json(const json& j, std::optional<T>& opt) {
            if (j.is_null()) {
                opt = std::nullopt;
            } else {
                opt = j.get<T>();
            }
        }
    };
}
#endif
// =====================================================================

"""
        )
        # ==========================================

        if namespace:
            f.write(f"namespace {namespace} {{\n\n")

        for macro in generated_macros:
            f.write(macro + "\n\n")

        if namespace:
            f.write(f"}} // namespace {namespace}\n")

    print(f"✅ 生成成功！共处理 {len(included_files)} 个文件。")


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("-d", "--dir", required=True)
    parser.add_argument("-o", "--output", required=True)
    parser.add_argument("-j", "--json-include", default="<nlohmann/json.hpp>")
    parser.add_argument("-ns", "--namespace", default="", help="C++ 命名空间")
    args = parser.parse_args()

    scan_and_generate(args.dir, args.output, args.json_include, args.namespace)
