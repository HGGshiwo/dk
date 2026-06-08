/* eslint-disable @typescript-eslint/no-explicit-any */
import Tree, { type TreeNode } from "./Tree";
import Table, { type TableColumnConfig, type TableEditValue } from "./Table";
import { useMemo, useRef, useState } from "react";
import { Input } from "antd";

interface GroupTableData {
  key: string; // 唯一标识
  group: string[]; // 分组依据
  [key: string]: string | number | boolean | string[]; // 动态字段
}
interface GroupTableConfig {
  columnConfig: TableColumnConfig[];
  titleKey?: string; // 树检索/渲染的字段
}
interface GroupTableProps extends GroupTableConfig {
  data: GroupTableData[];
  value?: TableEditValue;
  onChange?: (v: TableEditValue) => void;
  InputComp?: any;
}

export default function GroupTable({
  data,
  columnConfig,
  value,
  InputComp = Input,
  onChange: onTableChange,
  titleKey = "title",
}: GroupTableProps) {
  // 1. 状态管理：搜索关键词、选中的树节点key
  const [searchKeyword, setSearchKeyword] = useState("");
  const [selectedTreeNodeKey, setSelectedTreeNodeKey] = useState<string | null>(
    "__ALL__",
  );

  // 2. 构建树形数据的核心方法（抽离为独立hook，增加搜索过滤逻辑）
  const useTreeData = (items: GroupTableData[], keyword: string) => {
    type CacheDataType = {
      treeData: TreeNode[];
      tableData: GroupTableData[];
    };
    const treeDataCache = useRef<{ [key: string]: CacheDataType }>({});

    return useMemo(() => {
      // 生成缓存key（包含搜索关键词，避免缓存污染）
      const cacheKey = `${items.map((item) => item.key).join("_")}_${keyword}`;
      if (treeDataCache.current[cacheKey]) {
        return treeDataCache.current[cacheKey];
      }

      // 第一步：过滤符合搜索条件的原始数据
      const filteredItems = items.filter((item) => {
        if (!keyword) return true;
        // 搜索匹配规则：匹配title、group任意项、key（可根据需求调整）
        const matchTitle = (item[titleKey] as string)
          .toLowerCase()
          .includes(keyword.toLowerCase());
        const matchGroup = item.group.some((g) =>
          g.toLowerCase().includes(keyword.toLowerCase()),
        );
        const matchKey = item.key
          .toLowerCase()
          .includes(searchKeyword.toLowerCase());
        return matchTitle || matchGroup || matchKey;
      });

      // 第二步：构建树形结构
      const root: TreeNode = {
        [titleKey]: "全部",
        key: "__ALL__",
        children: [],
      };
      const nodeMap = new Map<string, TreeNode>([["__ALL__", root]]);

      // 预处理：过滤有效数据 + 按title排序
      const validItems = filteredItems
        .filter((item) => Array.isArray(item.group) && item.group.length > 0)
        .sort((a, b) => String(a[titleKey]).localeCompare(String(b[titleKey])));

      // 遍历构建树形结构（修复原逻辑中nodeKey重复的问题）
      validItems.forEach((item) => {
        const groupParts = item.group;
        let parentKey = "__ALL__";
        let currentCacheKey = "";

        groupParts.forEach((part: string, index: number) => {
          // 生成唯一的节点缓存key（避免同名称不同层级节点冲突）
          currentCacheKey = index === 0 ? part : `${currentCacheKey}_${part}`;
          // 叶子节点使用原始数据的key，非叶子节点使用缓存key（保证唯一性）
          const nodeKey =
            index === groupParts.length - 1 ? item.key : currentCacheKey;

          if (!nodeMap.has(currentCacheKey)) {
            const newNode: TreeNode = {
              [titleKey]: part,
              key: nodeKey,
              children: index === groupParts.length - 1 ? undefined : [], // 叶子节点无children
              // rawData: index === groupParts.length - 1 ? item : null, // 关联原始数据（方便后续筛选）
            };
            nodeMap.set(currentCacheKey, newNode);
            const parentNode = nodeMap.get(parentKey)!;
            parentNode.children = parentNode.children || [];
            parentNode.children.push(newNode);
          }

          parentKey = currentCacheKey;
        });
      });

      // 缓存结果
      const curData = { treeData: [root], tableData: validItems };
      treeDataCache.current[cacheKey] = curData;
      return curData;
    }, [items, keyword]);
  };

  // 3. 生成带搜索过滤的树形数据
  const { treeData, tableData } = useTreeData(data, searchKeyword);

  // 4. 筛选表格数据（结合选中节点 + 搜索条件）
  const getFilteredTableData = useMemo(() => {
    // 第一步：先过滤搜索结果
    const searchFilteredData = tableData.filter((item) => {
      if (!searchKeyword) return true;
      const matchTitle = (item[titleKey] as any)
        .toLowerCase()
        .includes(searchKeyword.toLowerCase());
      const matchGroup = item.group.some((g) =>
        g.toLowerCase().includes(searchKeyword.toLowerCase()),
      );
      const matchKey = item.key
        .toLowerCase()
        .includes(searchKeyword.toLowerCase());
      return matchTitle || matchGroup || matchKey;
    });

    // 第二步：根据选中节点过滤
    if (selectedTreeNodeKey === "__ALL__") {
      return searchFilteredData; // 全部显示
    }

    // 递归查找选中节点的所有叶子节点key
    const findLeafKeys = (
      node: TreeNode,
      targetKey: string | null,
    ): string[] => {
      // 找到目标节点
      if (node.key === targetKey) {
        // 叶子节点：返回自身key
        if (!node.children || node.children.length === 0) {
          return [node.key];
        }
        // 非叶子节点：递归收集所有子叶子节点key
        const leafKeys: string[] = [];
        const traverse = (childNode: TreeNode) => {
          if (!childNode.children || childNode.children.length === 0) {
            leafKeys.push(childNode.key);
          } else {
            childNode.children.forEach(traverse);
          }
        };
        node.children.forEach(traverse);
        return leafKeys;
      }

      // 未找到目标节点，递归子节点
      if (node.children && node.children.length > 0) {
        for (const child of node.children) {
          const result = findLeafKeys(child, targetKey);
          if (result.length > 0) return result;
        }
      }

      return [];
    };

    // 获取需要显示的叶子节点key列表
    const targetLeafKeys = findLeafKeys(treeData[0], selectedTreeNodeKey);
    // 筛选表格数据
    return searchFilteredData.filter((item) =>
      targetLeafKeys.includes(item.key),
    );
  }, [searchKeyword, selectedTreeNodeKey, tableData, titleKey, treeData]);

  // 5. 事件处理函数
  const handleSearchChange = (e: React.ChangeEvent<HTMLInputElement>) => {
    setSearchKeyword(e.target.value);
    // 搜索时重置选中节点为全部
    setSelectedTreeNodeKey("__ALL__");
  };

  const handleTreeSelect = (selectedKeys: string | null) => {
    setSelectedTreeNodeKey(selectedKeys);
  };

  const mobile = window.innerWidth < 500;

  return (
    <div className="flex flex-col ">
      <Input.Search
        style={{ marginBottom: 8 }}
        placeholder="Search"
        value={searchKeyword}
        onChange={handleSearchChange}
        allowClear // 增加清空按钮，提升体验
      />
      <div className="relative">
        {/* 传递选中状态和选择事件 */}
        <Tree
          style={{
            width: mobile ? "100%" : "200px",
            height: mobile ? "200px" : "100%",
            left: 0,
            top: 0,
            bottom: 0,
            position: "absolute",
          }}
          data={treeData}
          onSelect={handleTreeSelect}
          titleKey={titleKey}
        />
        {/* 传递过滤后的表格数据 */}
        <div className="bg-white">
          <Table
            style={{
              marginLeft: mobile ? 0 : "200px",
              marginTop: mobile ? "205px" : 0,
            }}
            value={value}
            onChange={onTableChange}
            columns={columnConfig}
            data={getFilteredTableData}
            InputComp={InputComp}
          />
        </div>
      </div>
    </div>
  );
}
