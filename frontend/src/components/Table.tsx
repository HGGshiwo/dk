/* eslint-disable @typescript-eslint/no-explicit-any */
import React, { useMemo, useCallback } from "react";
import { Table as AntTable } from "antd";
import type { TableProps as AntTableProps } from "antd/es/table";

export interface TableColumnConfig {
  key: string; // 列名（对应数据字段）
  title: string; // 列标题
  editable?: boolean; // 是否可编辑
  width?: number; // 列宽度
}

/** Table组件行数据 */
export interface TableRowData {
  key: string; // 唯一标识
  [key: string]: string | number | string[]; // 动态字段
}

// 受控组件的value类型：行ID -> 列名 -> (类型, 最新值)
export type TableEditValue = Record<string, Record<string, string | number>>;

interface TableProps {
  columns: TableColumnConfig[];
  data: TableRowData[];
  value?: TableEditValue; // 外部传入的受控值
  onChange?: (value: TableEditValue) => void; // 外部回调，通知值变化
  style?: React.CSSProperties;
  InputComp?: any; // 输入组件
  curIndex?: number; //当前高亮行
}

// 可编辑单元格组件（纯展示+事件转发，无内部状态依赖）
const EditableCell: React.FC<{
  text: string | number;
  record: TableRowData;
  columnKey: string;
  onSave: (rowId: string, colKey: string, value: string | number) => void;
  InputComp: any;
  width?: number;
}> = ({ text, record, columnKey, onSave, InputComp, width }) => {
  // 单元格仅维护编辑态（临时状态，不影响受控值）
  const [editing, setEditing] = React.useState(false);
  const [inputValue, setInputValue] = React.useState(text);
  const dataType = typeof text;
  // 非编辑状态时，同步外部传入的text（保证受控）
  React.useEffect(() => {
    if (!editing) {
      setInputValue(text);
    }
  }, [editing, text]);

  // 保存：转发给外部onChange
  const handleSave = useCallback(() => {
    if (inputValue !== text) {
      const value =
        dataType == "number" ? parseFloat(`${inputValue}`) : inputValue;
      onSave(record.key, columnKey, value);
    }
    setEditing(false);
  }, [inputValue, text, dataType, onSave, record.key, columnKey]);

  // ESC取消编辑，重置输入框
  const handleKeyDown = useCallback(
    (e: React.KeyboardEvent) => {
      if (e.key === "Escape") {
        setEditing(false);
        setInputValue(text);
      }
    },
    [text],
  );

  if (editing) {
    return (
      <div style={{ minWidth: `${width}px` }}>
        <InputComp
          value={inputValue as string}
          onChange={(e: any) => setInputValue(e.target.value)}
          autoFocus
          onBlur={handleSave}
          onPressEnter={handleSave}
          onKeyDown={handleKeyDown}
        />
      </div>
    );
  }

  return (
    <span
      onClick={() => setEditing(true)}
      style={{
        cursor: "pointer",
        display: "inline-block",
        width: width ? `${width}px` : undefined,
      }}
    >
      {text}
    </span>
  );
};

const Table: React.FC<TableProps> = ({
  columns,
  data,
  value = {}, // 外部受控值，默认空对象
  onChange,
  style = {},
  InputComp,
  curIndex,
}) => {
  // 1. 合并原始数据和外部受控值：用value覆盖原始数据，保证展示最新值
  const tableData = useMemo(() => {
    return data.map((row) => {
      const rowEdits = value[row.key] || {}; // 取当前行的编辑值
      return { ...row, ...rowEdits }; // 覆盖原始字段
    });
  }, [data, value]); // 依赖外部value，value变化则重新计算

  // 2. 处理单元格保存：生成新的value并调用外部onChange（纯函数，无副作用）
  const handleCellSave = useCallback(
    (rowId: string, colKey: string, newValue: string | number) => {
      if (!onChange) return; // 无回调则不处理

      // 不可变更新：基于当前外部value生成新值（保证受控）
      const newEditValue: TableEditValue = {
        ...value,
        [rowId]: {
          ...value[rowId], // 保留当前行其他列的编辑值
          [colKey]: newValue, // 更新当前列的值
        },
      };

      // 通知外部更新value
      onChange(newEditValue);
    },
    [value, onChange], // 依赖外部value和onChange
  );

  // 3. 渲染单元格：转发保存事件到handleCellSave
  const renderCell = useCallback(
    (
      text: string | number,
      record: TableRowData,
      col: TableColumnConfig,
      editWidth: number,
    ) => {
      if (col.editable) {
        return (
          <EditableCell
            text={text}
            record={record}
            columnKey={col.key}
            onSave={handleCellSave}
            InputComp={InputComp}
            width={editWidth}
          />
        );
      }
      return Array.isArray(text) ? text.join("\n") : text;
    },
    [handleCellSave, InputComp],
  );

  // 4. 转换为Antd Table列配置
  const antdColumns: AntTableProps<TableRowData>["columns"] = useMemo(
    () =>
      columns.map((col) => {
        const editWidth = col.width ? col.width : 80;
        return {
          title: col.title,
          dataIndex: col.key,
          key: col.key,
          width: col.editable ? editWidth : col.width,
          render: (text: string | number, record: TableRowData) =>
            renderCell(text, record, col, editWidth),
        };
      }),
    [columns, renderCell],
  );

  return (
    <AntTable
      classNames={{
        content: "my-classname",
      }}
      styles={{
        content: {
          backgroundColor: "white",
          height: "300px",
          overflow: "auto",
        },
      }}
      size="small"
      columns={antdColumns}
      dataSource={tableData}
      rowKey="id"
      // scroll={{ x: "auto", y: "auto" }} // 会导致高度失效
      style={{ ...style }}
      scroll={{ x: true }}
      rowClassName={(_record, index) =>
        index === curIndex ? "highlight-row" : ""
      }
      onRow={(_record, index) =>
        index === curIndex
          ? {
              style: {
                backgroundColor: "#ffe6a7",
              },
            }
          : {}
      }
    />
  );
};

export default Table;
