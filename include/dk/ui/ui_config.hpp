#pragma once
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <vector>

using json = nlohmann::json;

// ==========================================
// 1. 枚举定义 (Enum)
// ==========================================
// @JSON_ENABLE
enum class ButtonEventType {
    TOAST,  // @JSON("toast")
    MODAL,  // @JSON("modal")
    COPY    // @JSON("copy")
};

// @JSON_ENABLE
enum class LogboxDataType {
    ERROR_LOG,  // @JSON("error")
    INFO,       // @JSON("info")
    WARN,       // @JSON("warn")
    DEBUG_LOG,  // @JSON("debug")
    EVENT       // @JSON("event")
};

// @JSON_ENABLE
enum class FormItemType {
    SLIDER,       // @JSON("slider")
    INPUT,        // @JSON("input")
    NUMBER,       // @JSON("number")
    SELECT,       // @JSON("select")
    CHECKBOX,     // @JSON("checkbox")
    SWITCH_ITEM,  // @JSON("switch")
    RADIO,        // @JSON("radio")
    GROUP_TABLE   // @JSON("group_table")
};

// ==========================================
// 2. 基类配置 (Base)
// ==========================================
// @JSON_ENABLE
struct BaseUIConfig {
    std::string key;
    std::optional<std::string> config_id;
    std::string config_type;
};

// @JSON_ENABLE
struct BaseFormItemConfig {
    std::string key;
    std::string name;
    FormItemType type;
};

// @JSON_ENABLE
struct BaseButtonConfig : public BaseUIConfig {
    // @JSON_BASE(BaseUIConfig)
    std::string key;
    std::optional<std::string> name;
    int order = 9999;
    std::optional<json> target;  // 存储多态组件(ToastConfig/FormConfig等)的JSON形式
};

// ==========================================
// 3. 表单控件配置 (Form Items)
// ==========================================
// @JSON_ENABLE
struct InputFormItemConfig : public BaseFormItemConfig {
    // @JSON_BASE(BaseFormItemConfig)
    std::optional<std::string> placeholder;
    std::optional<int> maxlength;
    std::optional<std::string> default_val;  // @JSON("default")
    std::optional<std::string> transform;

    InputFormItemConfig() { type = FormItemType::INPUT; }
};

// @JSON_ENABLE
struct NumberFormItemConfig : public BaseFormItemConfig {
    // @JSON_BASE(BaseFormItemConfig)
    std::optional<int> min;
    std::optional<int> max;
    std::optional<int> default_val;  // @JSON("default")
    std::optional<int> precision;

    NumberFormItemConfig() { type = FormItemType::NUMBER; }
};

// @JSON_ENABLE
struct OptionConfig {
    std::string key;
    std::string name;
};

// @JSON_ENABLE
struct RadioFormItemConfig : public BaseFormItemConfig {
    // @JSON_BASE(BaseFormItemConfig)
    std::vector<OptionConfig> options;
    std::optional<std::string> default_val;  // @JSON("default")

    RadioFormItemConfig() { type = FormItemType::RADIO; }
};

// ==========================================
// 4. 表格配置
// ==========================================
// @JSON_ENABLE
struct TableColumnConfig {
    std::string key;
    std::string title;
    std::optional<bool> editable = false;
    std::optional<int> width;
};

// @JSON_ENABLE
struct GroupTableFormItemConfig : public BaseFormItemConfig {
    // @JSON_BASE(BaseFormItemConfig)
    std::vector<TableColumnConfig> columns;
    std::string titleKey = "title";

    GroupTableFormItemConfig() { type = FormItemType::GROUP_TABLE; }
};

// @JSON_ENABLE
struct TableConfig : public BaseUIConfig {
    // @JSON_BASE(BaseUIConfig)
    std::vector<TableColumnConfig> columns;
    std::optional<std::string> curIndexKey;

    TableConfig() { config_type = "table"; }
};

// ==========================================
// 5. 动作/弹窗配置
// ==========================================
// @JSON_ENABLE
struct ToastConfig : public BaseUIConfig {
    // @JSON_BASE(BaseUIConfig)
    std::optional<std::string> url;
    std::optional<std::string> method;
    std::optional<json> data;

    ToastConfig() { config_type = "toast"; }
};

// @JSON_ENABLE
struct CopyConfig : public BaseUIConfig {
    // @JSON_BASE(BaseUIConfig)
    std::optional<std::string> url;
    std::optional<std::string> method;

    CopyConfig() { config_type = "copy"; }
};

// ==========================================
// 6. 按钮与组件配置
// ==========================================
// @JSON_ENABLE
struct InnerButtonConfig : public BaseButtonConfig {
    // @JSON_BASE(BaseButtonConfig)
};

// @JSON_ENABLE
struct PrimaryButtonConfig : public BaseButtonConfig {
    // @JSON_BASE(BaseButtonConfig)
};

// @JSON_ENABLE
struct FormConfig : public BaseUIConfig {
    // @JSON_BASE(BaseUIConfig)
    std::string title = "表单名称";
    std::optional<std::string> url;
    std::optional<std::string> method;
    std::vector<json> items;  // 多态控件字典
    std::optional<InnerButtonConfig> submit;
    std::optional<InnerButtonConfig> on_change;

    FormConfig() { config_type = "form"; }
};

// @JSON_ENABLE
struct StatusConfig : public BaseUIConfig {
    // @JSON_BASE(BaseUIConfig)
    std::string key;
    std::optional<std::string> name;
    std::string default_val = "未知";  // @JSON("default")
    bool collapse = false;

    StatusConfig() { config_type = "state"; }
};

// ==========================================
// 7. 根对象配置 (对应最终的 yaml/json 文件结构)
// ==========================================
// @JSON_ENABLE
struct GlobalStore {
    std::vector<StatusConfig> state;
    std::vector<PrimaryButtonConfig> button;
    std::vector<TableConfig> table;
};