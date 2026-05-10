import { useAppStore } from "../store/useAppStore";
import { CollapseCard } from "./CollapseCard";

export interface StateItemConfig {
  key: string;
  name: string; // 显示文字
  default: string; // 默认值
  order?: number; // 排序，越小越靠前，默认999
  collapse?: boolean; // 是否默认折叠（不展开时显示）
}

const DisplayItem = ({ cfg }: { cfg: StateItemConfig; itemKey: string }) => {
  const value = useAppStore((state) => state.stateData[cfg.key]);

  return (
    <div
      key={cfg.key}
      className="rounded-lg shadow-sm bg-slate-50
                          p-2 sm:p-3 md:p-3 lg:p-3
                          h-full flex flex-col justify-start"
    >
      <div className="text-xs sm:text-sm text-gray-500 mb-1 truncate">
        {cfg.name}
      </div>
      <div className="text-base sm:text-lg font-bold text-gray-800 break-all">
        {String(value)}
      </div>
    </div>
  );
};

export const StateDisplay = () => {
  const config = useAppStore(state => state.config);
  const stateConfig = config?.state || [];

  return (
    <CollapseCard
      title="设备状态展示"
      className="mb-2 w-full"
      variant="borderless"
      config={stateConfig}
      renderItem={(key, cfg) => (
        <DisplayItem itemKey={key} cfg={cfg as StateItemConfig} />
      )}
    />
  );
};
