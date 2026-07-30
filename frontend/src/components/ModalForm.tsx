/* eslint-disable @typescript-eslint/no-explicit-any */
import {
  Modal,
  Form,
  Input,
  InputNumber,
  Button,
  Slider,
  Switch,
  Select,
  Radio,
  message,
} from "antd";
import { createRoot } from "react-dom/client";
import { httpRequest } from "../utils";
import { handleButtonClick, type ButtonItemConfig } from "./ButtonGroup";
import { useCallback, useEffect, useState } from "react";
import GroupTable from "./GroupTable";

export interface FormItemConfig {
  name: string; // 表单标签名
  key: string;
  id: string; // 表单字段唯一标识
  type:
    | "input"
    | "slider"
    | "number"
    | "switch"
    | "select"
    | "radio"
    | "group_table";

  default?: any; // 字段默认值
  max?: number; // 数值/滑块最大值
  min?: number; // 数值/滑块最小值
  step?: number; // 数值/滑块步长

  options?: any[]; // 下拉框选项
  required?: boolean; // 是否必填（扩展字段）
  transform?: "json" | "JSON"; //需要转换的格式
  columns?: Record<string, Record<string, any>>;
  titleKey?: string;
}

export interface FormConfig {
  config_type: "form";
  title?: string; // 弹窗标题
  method?: string; // 初始值请求方法
  url?: string; // 初始值请求地址
  items: FormItemConfig[]; // 表单项配置
  submit?: ButtonItemConfig; // 确认提交配置
  on_change?: ButtonItemConfig; // 实时提交配置
  initialModalVisible?: boolean; // 弹窗初始显示状态
}

export const getInput = (item: FormItemConfig, res: Record<string, any>) => {
  switch (item.type) {
    case "input":
      return <Input placeholder={`请输入${res?.name || item.name}`} />;
    case "number":
      return (
        <InputNumber
          max={res?.max || item.max}
          min={res?.min || item.min}
          step={res?.step || item.step}
          placeholder={`请输入${res?.name || item.name}`}
          style={{ width: "100%" }}
          mode="spinner"
        />
      );
    case "slider":
      return (
        <Slider
          max={res?.max || item.max}
          min={res?.min || item.min}
          step={res?.step || item.step}
        />
      );
    case "switch":
      return <Switch />;
    case "select":
      return (
        <Select
          options={
            (res?.options || item.options || []).map(
              (option: any) => ({
                value: option.key,
                label: option,
              }),
            ) || []
          }
          placeholder={`请选择${res?.name || item.name}`}
        />
      );
    case "radio":
      return (
        <Radio.Group>
          {(res?.options || item.options || {}).map(
            (option: any) => (
              <Radio value={option.key}>{option.name}</Radio>
            ),
          )}
        </Radio.Group>
      );
    case "group_table":
      // 测试用初始参数数据（包含多级group、不同类型值）
      console.log(111, res?.titleKey || item.titleKey);
      return (
        <GroupTable
          data={res?._value || []}
          // 可选：自定义列配置（注释掉则用默认列）
          columnConfig={Object.entries<any>(
            res?.columns || item.columns || {},
          ).map(([key, item]) => ({ key, ...item }))}
          titleKey={res?.titleKey || item.titleKey}
        />
      );
    default:
      return null;
  }
};

// 核心：表单弹窗组件（所有Hooks都放在这里面）
// eslint-disable-next-line react-refresh/only-export-components
const FormModal = ({
  formConfig,
  onDestroy, // 组件销毁时的回调
}: {
  formConfig: FormConfig;
  onDestroy: () => void;
}) => {
  // 1. 内部管理核心状态

  const [form] = Form.useForm<Record<string, any>>();
  const [modalVisible, setModalVisible] = useState<boolean>(
    formConfig.initialModalVisible ?? true, // 调用时默认显示
  );

  const [, setModalFormData] = useState<Record<string, any>>({});

  const [initialValues, setInitialValues] = useState<Record<string, any>>({});
  const [extraConfig, setExtraConfig] = useState<Record<string, any>>({}); // 支持接口直接返回默认值，或者是value+其他表单的props
  const [loading, setLoading] = useState<boolean>(true);

  // 辅助函数：从FormConfig.items中提取default值

  const getDefaultValues = useCallback((): Record<string, any> => {
    return formConfig.items.reduce(
      (acc, item) => {
        acc[item.key] = item.default ?? (item.type === "switch" ? false : "");
        return acc;
      },

      {} as Record<string, any>,
    );
  }, [formConfig.items]);

  // 2. 处理初始值逻辑：接口获取 或 解析item.default
  useEffect(() => {
    (async () => {
      // 清空之前的状态
      setModalFormData({});
      setInitialValues({});
      const validData = getDefaultValues() || {};
      const config: Record<string, any> = {}; // 记录接口可能传入的配置

      // 若配置了url和method，请求接口获取初始值
      if (formConfig.url && formConfig.method) {
        try {
          const res: any = await httpRequest(formConfig.method, formConfig.url);
          Object.entries(res).map(([key, item]) => {
            let value = item;
            if (typeof item == "object") {
              const { value: tmpValue, ...newItem } = item as any;
              config[key] = newItem;
              value = tmpValue;
            }
            validData[key] = value;
          });
        } catch (error) {
          console.error("获取表单初始值失败：", error);
        }
      }
      // 无接口时，直接解析item的default值
      setExtraConfig(config);
      setInitialValues(validData);
      setModalFormData(validData);
      // form.setFieldsValue(validData);
      setLoading(false);
    })();
  }, [formConfig.method, formConfig.url, getDefaultValues]);

  // 组件卸载时清理
  useEffect(() => {
    return () => {
      form.resetFields();
    };
  }, [form]);

  // 4. 通用提交函数（调用handleButtonClick）
  const submitForm = useCallback(
    async (params: Record<string, any>, buttonConfig: ButtonItemConfig) => {
      try {
        await handleButtonClick(buttonConfig, params);
      } catch (error) {
        console.error(`提交失败（${buttonConfig.name}）：`, error);
      }
    },
    [],
  );
  // 3. 表单值变化处理：更新内部状态 + 实时提交（若配置on_change）
  const onFormChange = useCallback(
    (_changedValues: Record<string, any>, allValues: Record<string, any>) => {
      // 更新内部表单数据状态
      setModalFormData(allValues);

      // 若配置了on_change，立即提交
      if (formConfig.on_change) {
        submitForm(allValues, formConfig.on_change);
      }
    },
    [formConfig.on_change, submitForm],
  );

  // 5. 确认按钮点击逻辑：表单校验 + 提交（若配置submit）
  const onConfirm = useCallback(async () => {
    try {
      // 先做表单校验
      const validValues = await form.validateFields();

      // 若配置了submit，提交表单参数
      if (formConfig.submit) {
        Object.entries(validValues).forEach(([key, item], idx) => {
          switch (formConfig.items[idx].transform) {
            case "json":
            case "JSON":
              try {
                validValues[key] = JSON.parse(item);
              } catch (e) {
                message.error(`${key}转为json时出错: ${e}`);
                return;
              }
              break;
          }
        });
        await submitForm(validValues, formConfig.submit);
      }

      // 提交成功后关闭弹窗并销毁组件
      setModalVisible(false);
      onDestroy();
    } catch (error) {
      console.error("表单校验/提交失败：", error);
    }
  }, [form, formConfig.submit, formConfig.items, onDestroy, submitForm]);

  // 6. 取消按钮点击逻辑：关闭弹窗并销毁组件
  const onCancel = useCallback(() => {
    setModalVisible(false);
    onDestroy();
  }, [onDestroy]);

  // 8. 渲染单个表单项
  const renderFormItem = useCallback(
    (key: string, item: FormItemConfig) => {
      const rules = item.required
        ? [{ required: true, message: `请输入/选择${item.name}` }]
        : undefined;
      return (
        <Form.Item
          key={key}
          label={item.name}
          name={key}
          rules={rules}
          valuePropName={item.type === "switch" ? "checked" : "value"}
        >
          {getInput(item, extraConfig?.[key])}
        </Form.Item>
      );
    },
    [extraConfig],
  );

  // 弹窗标题默认值
  const modalTitle = formConfig.title;

  // 渲染最终组件
  return (
    <Modal
      loading={loading}
      title={modalTitle}
      open={modalVisible}
      onCancel={onCancel}
      footer={[
        <Button key="cancel" onClick={onCancel}>
          取消
        </Button>,
        <Button
          key="confirm"
          type="primary"
          onClick={onConfirm}
          disabled={!formConfig.submit} // 无submit配置时禁用确认按钮
        >
          {formConfig.submit?.name || "确定"}
        </Button>,
      ]}
      width={"fit-content"}
      style={{ minWidth: "300px" }}
      destroyOnHidden // 关闭弹窗时销毁表单，避免缓存
      maskClosable={false} // 点击遮罩层不关闭，防止误操作
    >
      <Form
        form={form}
        onValuesChange={onFormChange}
        initialValues={initialValues}
        preserve={false}
        layout="horizontal"
      >
        {formConfig.items.map((item) =>
          renderFormItem(item.key, item),
        )}
      </Form>
    </Modal>
  );
};

/**
 * 触发表单弹窗显示的函数（无Hooks，仅负责动态挂载组件）
 * @param formConfig 完整的表单配置
 */
// eslint-disable-next-line @typescript-eslint/no-unused-vars
export const renderForm = (formConfig: FormConfig, _params: any) => {
  // 1. 检查并创建唯一的DOM容器
  let container = document.getElementById(`form-modal`);

  // 如果已存在，先销毁旧的组件
  if (container) {
    const root = createRoot(container);
    root.unmount();
    container.remove();
  }

  // 2. 创建新的DOM容器并添加到body
  container = document.createElement("div");
  container.id = `form-modal`;
  document.body.appendChild(container);

  // 3. 定义组件销毁回调
  const destroyComponent = () => {
    const root = createRoot(container!);
    root.unmount();
    container!.remove();
  };

  // 4. 动态挂载FormModal组件
  const root = createRoot(container);
  root.render(
    <FormModal formConfig={formConfig} onDestroy={destroyComponent} />,
  );
};
