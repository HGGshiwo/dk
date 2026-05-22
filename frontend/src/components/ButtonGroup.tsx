import { Button, Tooltip, Card, Tabs } from "antd";
import { useAppStore } from "../store/useAppStore";
import { message } from "antd";
import { renderForm, type FormConfig } from "./ModalForm";
import { renderToast, type ToastConfig } from "./Toast";
import { copyToClip, type CopyConfig } from "./Copy";
import { useState, useEffect } from "react";

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
  group?: string; // 分组名称
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
  const [activeTab, setActiveTab] = useState<string>("");
  const [currentPages, setCurrentPages] = useState<Record<string, number>>({});
  const [buttonsPerPage, setButtonsPerPage] = useState(9);

  // 根据屏幕宽度计算每页显示的按钮数量
  useEffect(() => {
    const updateButtonsPerPage = () => {
      const width = window.innerWidth;
      let cols = 3; // 默认3列
      
      if (width >= 1920) {
        cols = 16; // 超大屏：6列，每页18个按钮
      } else if (width >= 1440) {
        cols = 14; // 大屏：5列，每页15个按钮
      } else if (width >= 1024) {
        cols = 8; // 中屏：4列，每页12个按钮
      } else if (width >= 768) {
        cols = 6; // 平板：3列，每页9个按钮
      } else {
        cols = 3; // 移动端：3列，每页9个按钮
      }
      
      const rows = 3; // 固定3行
      setButtonsPerPage(cols * rows);
    };

    updateButtonsPerPage();
    window.addEventListener('resize', updateButtonsPerPage);
    return () => window.removeEventListener('resize', updateButtonsPerPage);
  }, []);

  // 按 group 字段分组
  const groupedButtons = config.reduce((acc, item) => {
    const group = item.group || "默认";
    if (!acc[group]) {
      acc[group] = [];
    }
    acc[group].push(item);
    return acc;
  }, {} as Record<string, ButtonItemConfig[]>);

  const groups = Object.keys(groupedButtons);
  
  // 设置默认激活的 tab
  if (activeTab === "" && groups.length > 0) {
    setActiveTab(groups[0]);
  }

  // 计算固定高度：3行按钮 + 分页器
  const buttonHeight = 32;
  const rowGap = 8;
  const cardPadding = 12;
  const tabBarHeight = groups.length > 1 ? 40 : 0;
  const paginationHeight = 32;
  
  const rows = 3; // 固定3行
  const contentHeight = rows * buttonHeight + (rows - 1) * rowGap;
  const totalHeight = contentHeight + cardPadding * 2 + tabBarHeight + paginationHeight;

  // 计算列数
  const cols = Math.ceil(buttonsPerPage / rows);

  // 渲染按钮网格
  const renderButtonGrid = (buttons: ButtonItemConfig[], groupKey: string) => {
    const currentPage = currentPages[groupKey] || 1;
    const totalPages = Math.ceil(buttons.length / buttonsPerPage);
    const startIndex = (currentPage - 1) * buttonsPerPage;
    const endIndex = startIndex + buttonsPerPage;
    const currentButtons = buttons.slice(startIndex, endIndex);

    return (
      <div style={{ 
        height: `${contentHeight + paginationHeight}px`,
        display: 'flex',
        flexDirection: 'column',
        justifyContent: 'space-between'
      }}>
        <div 
          style={{ 
            display: 'grid',
            gridTemplateColumns: `repeat(${cols}, 1fr)`,
            gap: '8px'
          }}
        >
          {currentButtons.map((item) => (
            <Tooltip key={item.key} title={item.tip} placement="top">
              <Button
                type="primary"
                // eslint-disable-next-line @typescript-eslint/no-explicit-any
                onClick={() => handleButtonClick(item as any)}
                size="small"
                className="w-full"
                style={{ fontSize: "12px", padding: "4px 8px", height: "32px" }}
              >
                {item.name}
              </Button>
            </Tooltip>
          ))}
        </div>
        
        <div style={{ display: 'flex', justifyContent: 'center', alignItems: 'center', height: `${paginationHeight}px` }}>
          {totalPages > 1 && (
            <div style={{ display: 'flex', gap: '8px' }}>
              {Array.from({ length: totalPages }, (_, index) => (
                <div
                  key={index}
                  onClick={() => {
                    setCurrentPages(prev => ({ ...prev, [groupKey]: index + 1 }));
                  }}
                  style={{
                    width: '8px',
                    height: '8px',
                    borderRadius: '50%',
                    backgroundColor: (index + 1) === currentPage ? '#1890ff' : '#d9d9d9',
                    cursor: 'pointer',
                    transition: 'all 0.3s',
                    boxShadow: (index + 1) === currentPage ? '0 0 4px rgba(24, 144, 255, 0.6)' : 'none'
                  }}
                />
              ))}
            </div>
          )}
        </div>
      </div>
    );
  };

  // 如果只有一个分组，不显示 Tab，直接显示按钮
  if (groups.length === 1) {
    return (
      <Card
        className="w-full"
        variant="borderless"
        style={{ 
          boxShadow: "0 4px 16px rgba(0,0,0,0.15)",
          height: `${totalHeight}px`
        }}
        bodyStyle={{ padding: "12px" }}
      >
        {renderButtonGrid(config, "default")}
      </Card>
    );
  }

  // 多个分组时显示 Tab
  const tabItems = groups.map((group) => ({
    key: group,
    label: group,
    children: renderButtonGrid(groupedButtons[group], group),
  }));

  return (
    <Card
      className="w-full"
      variant="borderless"
      style={{ 
        boxShadow: "0 4px 16px rgba(0,0,0,0.15)",
        height: `${totalHeight}px`
      }}
      bodyStyle={{ padding: "8px" }}
    >
      <Tabs
        activeKey={activeTab}
        onChange={setActiveTab}
        items={tabItems}
        size="middle"
        tabBarStyle={{ marginBottom: "8px" }}
      />
    </Card>
  );
};
