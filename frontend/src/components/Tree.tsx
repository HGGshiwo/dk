/* eslint-disable @typescript-eslint/no-explicit-any */
/* eslint-disable react-hooks/refs */
import React, { useEffect, useRef, useState, type ReactElement } from "react";
import { RightOutlined } from "@ant-design/icons";

// 多级菜单节点类型
export type TreeNode = {
  [key: string]: string | TreeNode[] | undefined;
  children?: TreeNode[];
  key: string;
};

// Tree组件Props
interface TreeProps {
  data: TreeNode[];
  titleKey?: string;
  onSelect: (
    selectedKey: string | null,
    selectedLeafKey: string | null,
  ) => void;
  showLine?: boolean; // 是否显示层级线
  icon?: ReactElement; // 自定义箭头图标
  style?: React.CSSProperties;
}

// 递归渲染树形节点
interface TreeItemComponentProps {
  node: TreeNode;
  titleKey: string;
  selectedKey: string | null;
  selectedLeafKey: string | null;
  onSelectNode?: (key: string, isLeaf: boolean) => void;
  level?: number;
  showLine?: boolean;
  icon?: any;
  onToggleExpand?: (key: string) => void;
  expandedKeys: string[];
}

const TreeItemComponent: React.FC<TreeItemComponentProps> = ({
  node,
  titleKey,
  selectedKey,
  selectedLeafKey,
  onSelectNode,
  level = 0,
  showLine = false,
  icon: CustomIcon = RightOutlined,
  onToggleExpand,
  expandedKeys,
}) => {
  const hasChildren = node.children && node.children.length > 0;
  const isLeaf = !hasChildren;
  const isExpanded = expandedKeys.includes(node.key);

  // 节点选中样式判断
  const isNodeSelected = isLeaf
    ? selectedLeafKey === node.key
    : selectedKey === node.key && selectedLeafKey === null;

  // 处理节点点击（同时实现选择和展开/折叠）
  const handleClickNode = (e: React.MouseEvent) => {
    e.stopPropagation();

    // 非叶子节点先切换展开状态
    if (!isLeaf && onToggleExpand) {
      onToggleExpand(node.key);
    }

    // 执行节点选择逻辑
    onSelectNode?.(node.key, isLeaf);
  };

  // 计算层级缩进
  const baseIndent = 8;
  const levelIndent = level * baseIndent;
  const totalIndent = baseIndent + levelIndent;
  const [top, setTop] = useState("0px");

  let bgColor = "";
  if (!isLeaf) bgColor = `bg-white`;
  if (isNodeSelected) {
    bgColor = "bg-blue-100 text-blue-700";
  }
  const titleDiv = useRef<null | HTMLDivElement>(null);
  useEffect(() => {
    if (!titleDiv.current) return;
    setTop(`${titleDiv.current.offsetHeight}px`);
  }, [titleDiv.current]);

  return (
    <li className="py-1 relative">
      {/* 修复点1：层级线仅在节点展开且有子节点时显示 */}
      {showLine && isExpanded && hasChildren && (
        <div
          className="absolute left-6 bottom-0 border-l border-gray-400"
          style={{
            zIndex: 3,
            top: top,
          }}
        />
      )}

      {/* 节点标题行 */}
      <div
        ref={titleDiv}
        className={`flex items-center gap-2 py-1.5 rounded-md cursor-pointer transition-colors hover:bg-gray-100 ${bgColor}`}
        style={{
          padding: `8px 0px 8px ${totalIndent}px`,
          position: "relative",
          zIndex: 1,
        }}
        onClick={handleClickNode}
      >
        {/* 箭头图标 - 单图标旋转实现展开/折叠 */}
        {!isLeaf && (
          <span
            className={`scale-80 inline-block transition-transform duration-200 ease-in-out ${
              isExpanded ? "rotate-90" : ""
            } text-gray-600`}
            style={{ width: "16px", height: "16px" }}
          >
            <CustomIcon />
          </span>
        )}

        {/* 叶子节点显示空白占位，保持对齐 */}
        {isLeaf && (
          <span className="w-5 inline-block relative">
            {showLine && (
              <span className="inline-block absolute top-0.5 h-0 border-t border-gray-400"></span>
            )}
          </span>
        )}

        {/* 节点标题 */}
        <span className="flex-1">{node[titleKey] as string}</span>
      </div>

      {/* 非叶子节点且展开时，动态渲染子节点 */}
      {!isLeaf && isExpanded && node.children && (
        <ul className="mt-1 ml-4">
          {node.children.map((child) => (
            <TreeItemComponent
              titleKey={titleKey}
              key={child.key}
              node={child}
              selectedKey={selectedKey}
              selectedLeafKey={selectedLeafKey}
              onSelectNode={onSelectNode}
              level={level + 1}
              showLine={showLine}
              icon={CustomIcon}
              onToggleExpand={onToggleExpand}
              expandedKeys={expandedKeys}
            />
          ))}
        </ul>
      )}
    </li>
  );
};

// 核心Tree组件
const Tree: React.FC<TreeProps> = ({
  data,
  onSelect,
  icon,
  style = {},
  showLine = true,
  titleKey = "title",
}) => {
  // 状态管理
  const [selectedKey, setSelectedKey] = useState<string | null>(null);
  const [selectedLeafKey, setSelectedLeafKey] = useState<string | null>(null);
  const [expandedKeys, setExpandedKeys] = useState<string[]>([]); // 展开的节点key集合

  // 切换节点展开/折叠状态
  const handleToggleExpand = (key: string) => {
    setExpandedKeys((prev) =>
      prev.includes(key) ? prev.filter((k) => k !== key) : [...prev, key],
    );
  };

  // 处理节点选择
  const handleSelectNode = (key: string, isLeaf: boolean) => {
    if (isLeaf) {
      setSelectedKey(key);
      setSelectedLeafKey(key);
      onSelect(key, key);
    } else {
      setSelectedKey(key);
      setSelectedLeafKey(null);
      onSelect(key, null);
    }
  };

  // 处理"全部"选择
  const handleSelectAll = () => {
    setSelectedKey(null);
    setSelectedLeafKey(null);
    onSelect("__ALL__", null);
  };

  // 修复点2：定义根节点选中状态的正确逻辑
  const isRootSelected = !selectedKey && !selectedLeafKey;

  return (
    <div
      style={style}
      className="w-50 bg-white rounded-lg shadow-sm border border-gray-100 overflow-auto "
    >
      {/* 树形节点容器 */}
      <ul className="py-1 overflow-y-auto">
        <li className="py-1 relative">
          {/* 节点标题行 */}
          <div
            className={`flex items-center gap-2 py-1.5 rounded-md cursor-pointer transition-colors ${
              // 修复点2：使用正确的选中状态判断逻辑，替换硬编码的true
              isRootSelected ? "bg-blue-100 text-blue-700" : "hover:bg-gray-100"
            }`}
            style={{
              padding: `8px 0px 8px 8px`,
              position: "relative",
              zIndex: 1,
            }}
            onClick={handleSelectAll}
          >
            <span className="w-5 inline-block">
              {showLine && (
                <span className="inline-block w-1 h-1 bg-gray-200 rounded-full"></span>
              )}
            </span>

            {/* 节点标题 */}
            <span className="flex-1">{data[0][titleKey] as string}</span>
          </div>
        </li>
        {data[0].children?.map((node) => (
          <TreeItemComponent
           titleKey={titleKey}
            key={node.key}
            node={node}
            selectedKey={selectedKey}
            selectedLeafKey={selectedLeafKey}
            onSelectNode={handleSelectNode}
            showLine={showLine}
            icon={icon}
            onToggleExpand={handleToggleExpand}
            expandedKeys={expandedKeys}
          />
        ))}
      </ul>
    </div>
  );
};

export default Tree;
