/* eslint-disable @typescript-eslint/no-explicit-any */
import { Card } from "antd";
import { useAppStore } from "../store/useAppStore";
import Table, { type TableColumnConfig } from "./Table";
import { useMemo } from "react";

export interface TableGroupConfig {
  columns: TableColumnConfig[];
  curIndexKey?: string; // 使用ws这哪个数据作为高亮索引
  key: string;
}

export default function TableGroup() {
  const { config, stateData } = useAppStore();
  const tableConfig = useMemo(() => config?.table || [], [config?.table]);

  const hasData = useMemo(() => {
    return tableConfig.some(
      (item) => !!(stateData[item.key] as any[])?.length,
    );
  }, [stateData, tableConfig]);

  return (
    hasData && (
      <Card
        title="设备状态展示2"
        className="mb-6 w-full"
        variant="borderless"
        style={{ boxShadow: "0 2px 8px rgba(0,0,0,0.08)" }}
      >
        {tableConfig.map((item) => {
          const columns = item.columns;
          const curIndex = item.curIndexKey && stateData[item.curIndexKey];
          return (
            <Table
              key={item.key}
              data={(stateData[item.key] as any) || []}
              columns={columns}
              curIndex={curIndex as number}
            />
          );
        })}
      </Card>
    )
  );
}
