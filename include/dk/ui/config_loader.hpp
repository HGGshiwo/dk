#pragma once
#include <yaml-cpp/yaml.h>

#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>

#include "ui_config.hpp"

using json = nlohmann::json;

class ConfigLoader {
   private:
    // 递归：YAML 标量类型推断
    static json yaml_to_json(const YAML::Node& node) {
        json j;
        switch (node.Type()) {
            case YAML::NodeType::Null:
                j = nullptr;
                break;
            case YAML::NodeType::Scalar: {
                try {
                    return node.as<bool>();
                } catch (...) {
                }
                try {
                    return node.as<int64_t>();
                } catch (...) {
                }
                try {
                    return node.as<double>();
                } catch (...) {
                }
                return node.as<std::string>();
            }
            case YAML::NodeType::Sequence: {
                j = json::array();
                for (const auto& item : node) j.push_back(yaml_to_json(item));
                break;
            }
            case YAML::NodeType::Map: {
                j = json::object();
                for (const auto& kv : node) j[kv.first.as<std::string>()] = yaml_to_json(kv.second);
                break;
            }
            default:
                break;
        }
        return j;
    }

   public:
    /**
     * 核心流程: 读取 YAML -> 转为 JSON -> 注入 C++ 结构体补全默认值 -> 生成最终 JSON
     */
    static json load_and_complete(const std::string& yaml_path) {
        try {
            // 1. 读取 YAML
            YAML::Node yaml_root = YAML::LoadFile(yaml_path);

            // 2. 初步转为无类型推断缺失字段的 JSON
            json raw_json = yaml_to_json(yaml_root);
            // std::cout << raw_json.dump() << std::endl;
            // 3. 【核心】映射到 C++ Struct，由于缺失的字段在 C++ 中有默认值，此时默认值被保留！
            GlobalStore store = raw_json.get<GlobalStore>();

            // 4. 将补全后的 C++ 结构体转回为 JSON
            json final_json = store;
            return final_json;

        } catch (const std::exception& e) {
            std::cerr << "配置加载失败: " << e.what() << std::endl;
            throw;
        }
    }

    /**
     * 直接加载 yaml 并生成 json 文件
     */
    static void generate_json_file(const std::string& yaml_path, const std::string& json_path) {
        json final_json = load_and_complete(yaml_path);
        std::ofstream out(json_path);
        if (out.is_open()) {
            out << final_json.dump(4);  // 4 空格缩进格式化
            std::cout << "✅ 配置文件已生成: " << json_path << std::endl;
        } else {
            std::cerr << "❌ 无法写入文件: " << json_path << std::endl;
        }
    }
};