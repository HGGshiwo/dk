import { Button, Tooltip, Card, Row, Col } from "antd";
import { useAppStore } from "../store/useAppStore";
import { message } from "antd";
import { renderForm, type FormConfig } from "./ModalForm";
import { renderToast, type ToastConfig } from "./Toast";
import { copyToClip, type CopyConfig } from "./Copy";

// 弹窗表单字段配置接口
export interface FormItemConfig {
  key: string;
  name: string; // 显示名称
  type: "number" | "string" | "text"; // 表单类型
}

// 点击按钮配置接口
export interface ButtonItemConfig {
  target: FormConfig | ToastConfig | CopyConfig;
  name: string; // 按钮显示的名称
  order?: number;
  tip?: string;
  key: string;
}

// 按钮点击核心逻辑（原有逻辑不变，仅修改布局）

// eslint-disable-next-line react-refresh/only-export-components
export const handleButtonClick = async (
  item: ButtonItemConfig,
  // eslint-disable-next-line @typescript-eslint/no-explicit-any
  params?: any,
) => {
  try {
    switch (item.target?.config_type) {
      case "toast":
        await renderToast(item.target as ToastConfig, params);
        break;
      case "form":
        await renderForm(item.target as FormConfig, params);
        break;
      case "copy":
        await copyToClip(item.target as CopyConfig, params);
        break;
      default:
        // eslint-disable-next-line @typescript-eslint/no-explicit-any
        message.error(`不支持按钮类型: ${(item.target as any)?.config_type}!`);
    }
  } catch (error) {
    // message.error("操作失败，请重试");
    console.error(`按钮${item.name}操作失败：`, error);
  }
};

export const ButtonGroup = () => {
  const config = useAppStore((state) => state.config?.button) || [];
  return (
    <Card
      title="设备控制操作"
      className="mb-2 w-full"
      variant="borderless"
      style={{ boxShadow: "0 2px 8px rgba(0,0,0,0.08)" }}
    >
      {/* 优化：PC端撑满，移动端4列（更紧凑），平板3列，PC端6列/8列 */}
      <Row gutter={[12, 12]} className="w-full">
        {config.map((item) => (
          <Col
            key={item.key}
            xs={8} // 移动端：4列（320px屏也能放下）
            sm={8} // 小平板：3列
            md={6} // 平板：4列
            lg={4} // 小屏PC：6列
            xl={3} // 大屏PC：8列（适配几十个按钮）
          >
            <Tooltip title={item.tip} placement="top">
              <Button
                type="primary"
                // eslint-disable-next-line @typescript-eslint/no-explicit-any
                onClick={() => handleButtonClick(item as any)}
                size="large" // 按钮尺寸微调，更紧凑
                className="w-full"
              >
                {item.name}
              </Button>
              {/* <button
                onClick={() => handleButtonClick(key)}
                className="btn btn-primary"
              >
                {key}
              </button> */}
            </Tooltip>
          </Col>
        ))}
      </Row>
    </Card>
  );
};
