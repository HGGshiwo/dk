import { useAppStore } from "../store/useAppStore";
import { sortByOrder } from "../utils";

export interface StateItemConfig {
  key: string;
  name: string; // 显示文字
  default: string; // 默认值
  order?: number; // 排序，越小越靠前，默认999
  collapse?: boolean; // 是否默认折叠（不展开时显示）
  true_value?: string; // state_bool 类型：true 时显示的文字
  false_value?: string; // state_bool 类型：false 时显示的文字
  fixed?: number; // 数值固定小数位数，不够位数补0
}

interface DisplayItemProps {
  cfg: StateItemConfig;
  displayType: "state_bool" | "state_key_value" | "state_value";
}

const DisplayItem = ({ cfg, displayType }: DisplayItemProps) => {
  const value = useAppStore((state) => state.stateData[cfg.key]);

  // 格式化值：如果配置了fixed，将值固定到指定小数位数
  const formatValue = (val: any): string => {
    if (cfg.fixed !== undefined && cfg.fixed >= 0) {
      const numValue = Number(val);
      if (!isNaN(numValue)) {
        return numValue.toFixed(cfg.fixed);
      }
    }
    return String(val);
  };

  // 根据不同类型渲染不同内容
  const renderContent = () => {
    switch (displayType) {
      case "state_bool": {
        // 布尔类型：根据值显示 true_value 或 false_value
        const boolValue = Boolean(value);
        
        // 如果没有提供 true_value 和 false_value，显示圆点
        if (!cfg.true_value && !cfg.false_value) {
          return (
            <div className="flex items-center gap-2">
              <span className="text-xs text-white/80">{cfg.name}</span>
              <div 
                className="w-3 h-3 rounded-full"
                style={{ 
                  backgroundColor: boolValue ? '#52c41a' : '#ff4d4f',
                  boxShadow: boolValue 
                    ? '0 0 8px rgba(82, 196, 26, 0.6)' 
                    : '0 0 8px rgba(255, 77, 79, 0.6)'
                }}
              />
            </div>
          );
        }
        
        // 有自定义文字时显示文字
        const displayText = boolValue 
          ? (cfg.true_value || `${cfg.name}开`) 
          : (cfg.false_value || `${cfg.name}关`);
        return (
          <span className="text-sm font-semibold text-white">
            {displayText}
          </span>
        );
      }
      
      case "state_value": {
        // 只显示值
        return (
          <span className="text-sm font-semibold text-white">
            {formatValue(value)}
          </span>
        );
      }
      
      case "state_key_value":
      default: {
        // 显示 key: value
        return (
          <>
            <span className="text-xs text-white/80">{cfg.name}:</span>
            <span className="text-sm font-semibold text-white">
              {formatValue(value)}
            </span>
          </>
        );
      }
    }
  };

  return (
    <div
      key={cfg.key}
      className="flex items-center gap-2 px-3 py-1 bg-black/40 backdrop-blur-sm rounded-lg"
    >
      {renderContent()}
    </div>
  );
};

export const StateDisplay = () => {
  const config = useAppStore(state => state.config);
  
  // 从配置中读取三种类型的状态
  const stateKeyValue = (config?.state || []) as StateItemConfig[];
  const stateBool = (config?.state_bool || []) as StateItemConfig[];
  const stateValue = (config?.state_value || []) as StateItemConfig[];
  
  // 合并所有状态配置，并标记类型（按照 state_bool, state_value, state 的顺序）
  const allStates = [
    ...stateBool.map(item => ({ ...item, displayType: "state_bool" as const })),
    ...stateValue.map(item => ({ ...item, displayType: "state_value" as const })),
    ...stateKeyValue.map(item => ({ ...item, displayType: "state_key_value" as const })),
  ];
  
  // 排序并过滤
  const sortedConfig = sortByOrder(allStates);
  const displayItems = sortedConfig.filter((item) => item.collapse === false);

  return (
    <div className="flex flex-wrap gap-2 p-4">
      {displayItems.map((item) => (
        <DisplayItem 
          key={item.key} 
          cfg={item as StateItemConfig} 
          displayType={item.displayType}
        />
      ))}
    </div>
  );
};
