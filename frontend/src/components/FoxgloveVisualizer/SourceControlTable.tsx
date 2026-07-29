import React from "react";
import { Table, Button } from "antd";
import { EyeOutlined, EyeInvisibleOutlined, LockOutlined, UnlockOutlined, SyncOutlined } from "@ant-design/icons";
import type { SourceConfig } from "./useVisualizerData";

interface Props {
  sources: SourceConfig[];
  onToggleEnabled: (id: string, checked: boolean) => void;
  onToggleLock: (id: string, locked: boolean) => void;
  onResetAutoY: (id: string) => void;
}

export const SourceControlTable: React.FC<Props> = ({ sources, onToggleEnabled, onToggleLock, onResetAutoY }) => {
  const columns = [
    {
      title: "数据源",
      dataIndex: "name",
      key: "name",
      render: (text: string, record: SourceConfig) => (
        <div>
          <div className="font-bold text-xs flex items-center gap-1.5" style={{ color: record.color }}>
            {text}
          </div>
          <div className="text-[10px] text-slate-400 font-mono truncate max-w-[170px]" title={record.topic}>
            {record.topic}
          </div>
        </div>
      ),
    },
    {
      title: "显示",
      dataIndex: "enabled",
      key: "enabled",
      width: 40,
      align: "center" as const,
      render: (checked: boolean, record: SourceConfig) => (
        <Button
          type="text"
          size="small"
          className="p-0 border-0 flex items-center justify-center hover:bg-slate-100"
          icon={checked ? <EyeOutlined style={{ color: "#3b82f6", fontSize: "15px" }} /> : <EyeInvisibleOutlined style={{ color: "#94a3b8", fontSize: "15px" }} />}
          onClick={() => onToggleEnabled(record.id, !checked)}
        />
      ),
    },
    {
      title: "锁定状态",
      dataIndex: "locked",
      key: "locked",
      width: 40,
      align: "center" as const,
      render: (locked: boolean, record: SourceConfig) => {
        if (record.type !== "line") return <span className="text-slate-300">-</span>;
        return (
          <Button
            type="text"
            size="small"
            className="p-0 border-0 flex items-center justify-center hover:bg-slate-100"
            icon={locked ? <LockOutlined style={{ color: "#dc2626", fontSize: "14px" }} /> : <UnlockOutlined style={{ color: "#16a34a", fontSize: "14px" }} />}
            onClick={() => onToggleLock(record.id, !locked)}
          />
        );
      },
    },
    {
      title: "重置",
      dataIndex: "autoY",
      key: "autoY",
      width: 40,
      align: "center" as const,
      render: (autoY: boolean, record: SourceConfig) => {
        if (record.type !== "line") return <span className="text-slate-300">-</span>;
        return (
          <Button
            type="text"
            size="small"
            className={`p-0 border-0 flex items-center justify-center hover:bg-slate-100 ${autoY ? "text-blue-500 font-bold" : "text-slate-400"}`}
            icon={<SyncOutlined style={{ fontSize: "12px" }} />}
            onClick={() => onResetAutoY(record.id)}
          />
        );
      },
    },
  ];

  return (
    <div className="w-full md:w-[320px] bg-slate-50 border-r border-slate-200 flex flex-col h-[40vh] md:h-full overflow-hidden select-none">
      <div className="px-3 py-2 border-b border-slate-200 bg-slate-100 flex items-center justify-between">
        <span className="font-bold text-slate-700 text-xs">数据源控制列表</span>
      </div>
      <div className="flex-1 overflow-auto p-1.5">
        <Table dataSource={sources} columns={columns} rowKey="id" pagination={false} size="small" bordered={false} showHeader={false} className="bg-transparent" />
      </div>
    </div>
  );
};