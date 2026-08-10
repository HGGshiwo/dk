import React from "react";
import { Button, InputNumber, Radio, Space, Table } from "antd";
import type { ColumnsType } from "antd/es/table";
import type { Waypoint, FollowState } from "../types";
import { FollowControlPanel } from "./FollowControlPanel";

interface WaypointTablePanelProps {
  mode: "waypoint" | "follow";
  handleModeChange: (mode: "waypoint" | "follow") => void;
  coordinateType: "gps" | "local";
  setCoordinateType: (type: "gps" | "local") => void;
  displayWaypoints: Waypoint[];
  waypoints: Waypoint[];
  setWaypoints: React.Dispatch<React.SetStateAction<Waypoint[]>>;
  isEditing: boolean;
  setIsEditing: (editing: boolean) => void;
  pushToHistory: (waypoints: Waypoint[]) => void;
  undo: () => void;
  redo: () => void;
  canUndo: boolean;
  canRedo: boolean;
  clearWaypoints: () => void;
  submitWaypoints: () => void;
  handleCancelEdit: () => void;
  enuToGps: (x: number, y: number) => { lat: number; lon: number };
  gpsToEnu: (lat: number, lon: number) => { x: number; y: number };
  wp_idx?: number;
  pos_enu?: number[];
  cur_lat?: number;
  cur_lon?: number;
  followState: FollowState;
  setFollowState: React.Dispatch<React.SetStateAction<FollowState>>;
  stopFollow: () => void;
}

export const WaypointTablePanel: React.FC<WaypointTablePanelProps> = ({
  mode,
  handleModeChange,
  coordinateType,
  setCoordinateType,
  displayWaypoints,
  waypoints,
  setWaypoints,
  isEditing,
  setIsEditing,
  pushToHistory,
  undo,
  redo,
  canUndo,
  canRedo,
  clearWaypoints,
  submitWaypoints,
  handleCancelEdit,
  enuToGps,
  gpsToEnu,
  wp_idx,
  pos_enu,
  cur_lat,
  cur_lon,
  followState,
  setFollowState,
  stopFollow,
}) => {
  const moveWaypoint = (index: number, direction: "up" | "down") => {
    setIsEditing(true);
    if (index === 0) return;
    if (direction === "up" && index === 0) return;
    if (direction === "down" && index === waypoints.length - 1) return;
    const newWaypoints = [...waypoints];
    const targetIndex = direction === "up" ? index - 1 : index + 1;
    [newWaypoints[index], newWaypoints[targetIndex]] = [
      newWaypoints[targetIndex],
      newWaypoints[index],
    ];
    pushToHistory(newWaypoints);
    setWaypoints(newWaypoints);
  };

  // GPS 模式表格列
  const gpsColumns: ColumnsType<Waypoint> = [
    {
      title: "序号",
      dataIndex: "id",
      key: "index",
      render: (_, __, index) => index + 1,
      width: 50,
      align: "center",
    },
    {
      title: "经度",
      dataIndex: "lon",
      key: "lon",
      render: (val: number, record) => (
        <InputNumber
          value={Number(val.toFixed(7))}
          style={{ width: "100%" }}
          precision={7}
          step={0.00001}
          onChange={(value) => {
            setIsEditing(true);
            const newLon = Number(value);
            setWaypoints((prev) =>
              prev.map((wp) => {
                if (wp.id !== record.id) return wp;
                const enu = gpsToEnu(wp.lat, newLon);
                return { ...wp, lon: newLon, x: enu.x, y: enu.y };
              })
            );
          }}
        />
      ),
    },
    {
      title: "纬度",
      dataIndex: "lat",
      key: "lat",
      render: (val: number, record) => (
        <InputNumber
          value={Number(val.toFixed(7))}
          style={{ width: "100%" }}
          precision={7}
          step={0.00001}
          onChange={(value) => {
            setIsEditing(true);
            const newLat = Number(value);
            setWaypoints((prev) =>
              prev.map((wp) => {
                if (wp.id !== record.id) return wp;
                const enu = gpsToEnu(newLat, wp.lon);
                return { ...wp, lat: newLat, x: enu.x, y: enu.y };
              })
            );
          }}
        />
      ),
    },
    {
      title: "高度 (m)",
      dataIndex: "alt",
      key: "alt",
      render: (val: number, record) => (
        <InputNumber
          value={val}
          style={{ width: "100%" }}
          precision={1}
          step={1}
          min={0}
          onChange={(value) => {
            setIsEditing(true);
            const newAlt = Number(value);
            setWaypoints((prev) =>
              prev.map((wp) =>
                wp.id === record.id ? { ...wp, alt: newAlt, z: newAlt } : wp
              )
            );
          }}
        />
      ),
    },
    {
      title: "顺序",
      key: "order",
      width: 90,
      align: "center",
      render: (_, __, index) => (
        <Space size="small">
          <Button
            size="small"
            icon={<span>↑</span>}
            onClick={() => moveWaypoint(index, "up")}
            disabled={index === 0}
          />
          <Button
            size="small"
            icon={<span>↓</span>}
            onClick={() => moveWaypoint(index, "down")}
            disabled={index === displayWaypoints.length - 1}
          />
        </Space>
      ),
    },
    {
      title: "操作",
      key: "action",
      width: 70,
      align: "center",
      render: (_, record) => (
        <Button
          type="link"
          danger
          onClick={() => {
            setIsEditing(true);
            const next = waypoints.filter((wp) => wp.id !== record.id);
            pushToHistory(next);
            setWaypoints(next);
          }}
        >
          删除
        </Button>
      ),
    },
  ];

  // ENU 局部坐标系表格列
  const enuColumns: ColumnsType<Waypoint> = [
    {
      title: "序号",
      dataIndex: "id",
      key: "index",
      render: (_, __, index) => index + 1,
      width: 50,
      align: "center",
    },
    {
      title: "X (米)",
      dataIndex: "x",
      key: "x",
      render: (val: number, record) => (
        <InputNumber
          value={Number(val.toFixed(2))}
          style={{ width: "100%" }}
          precision={2}
          step={0.5}
          onChange={(value) => {
            setIsEditing(true);
            const newX = Number(value);
            setWaypoints((prev) =>
              prev.map((wp) => {
                if (wp.id !== record.id) return wp;
                const gps = enuToGps(newX, wp.y);
                return { ...wp, x: newX, lon: gps.lon, lat: gps.lat };
              })
            );
          }}
        />
      ),
    },
    {
      title: "Y (米)",
      dataIndex: "y",
      key: "y",
      render: (val: number, record) => (
        <InputNumber
          value={Number(val.toFixed(2))}
          style={{ width: "100%" }}
          precision={2}
          step={0.5}
          onChange={(value) => {
            setIsEditing(true);
            const newY = Number(value);
            setWaypoints((prev) =>
              prev.map((wp) => {
                if (wp.id !== record.id) return wp;
                const gps = enuToGps(wp.x, newY);
                return { ...wp, y: newY, lon: gps.lon, lat: gps.lat };
              })
            );
          }}
        />
      ),
    },
    {
      title: "Z (米)",
      dataIndex: "z",
      key: "z",
      render: (val: number, record) => (
        <InputNumber
          value={val}
          style={{ width: "100%" }}
          precision={1}
          step={1}
          min={0}
          onChange={(value) => {
            setIsEditing(true);
            const newZ = Number(value);
            setWaypoints((prev) =>
              prev.map((wp) =>
                wp.id === record.id ? { ...wp, z: newZ, alt: newZ } : wp
              )
            );
          }}
        />
      ),
    },
    {
      title: "顺序",
      key: "order",
      width: 90,
      align: "center",
      render: (_, __, index) => (
        <Space size="small">
          <Button
            size="small"
            icon={<span>↑</span>}
            onClick={() => moveWaypoint(index, "up")}
            disabled={index === 0}
          />
          <Button
            size="small"
            icon={<span>↓</span>}
            onClick={() => moveWaypoint(index, "down")}
            disabled={index === displayWaypoints.length - 1}
          />
        </Space>
      ),
    },
    {
      title: "操作",
      key: "action",
      width: 70,
      align: "center",
      render: (_, record) => (
        <Button
          type="link"
          danger
          onClick={() => {
            setIsEditing(true);
            const next = waypoints.filter((wp) => wp.id !== record.id);
            pushToHistory(next);
            setWaypoints(next);
          }}
        >
          删除
        </Button>
      ),
    },
  ];

  return (
    <div
      style={{
        position: "absolute",
        bottom: 0,
        left: 0,
        right: 0,
        height: "40vh",
        backgroundColor: "rgba(255, 255, 255, 0.95)",
        backdropFilter: "blur(8px)",
        boxShadow: "0 -4px 16px rgba(0,0,0,0.15)",
        zIndex: 25,
        padding: "16px",
        overflowY: "auto",
      }}
    >
      <div
        className="mode-switch"
        style={{ marginBottom: "16px", display: "flex", gap: "16px" }}
      >
        <Radio.Group
          value={mode}
          onChange={(e) => handleModeChange(e.target.value)}
          buttonStyle="solid"
        >
          <Radio.Button value="waypoint">航点模式</Radio.Button>
          <Radio.Button value="follow">跟随模式</Radio.Button>
        </Radio.Group>

        <Radio.Group
          value={coordinateType}
          onChange={(e) => setCoordinateType(e.target.value)}
          buttonStyle="solid"
        >
          <Radio.Button value="local">局部坐标系 (ENU)</Radio.Button>
          <Radio.Button value="gps">GPS坐标系</Radio.Button>
        </Radio.Group>
      </div>

      {mode === "waypoint" && (
        <>
          <div
            className="table-header"
            style={{
              marginBottom: "8px",
              display: "flex",
              alignItems: "center",
              justifyContent: "space-between",
            }}
          >
            <div style={{ display: "flex", alignItems: "center", gap: "16px" }}>
              <span style={{ fontWeight: 600 }}>航点列表</span>
              {coordinateType === "local" && pos_enu && pos_enu.length >= 3 && (
                <span style={{ fontSize: "13px", color: "#1890ff", fontWeight: 500 }}>
                  当前位置 (ENU): X: {pos_enu[0].toFixed(2)}, Y: {pos_enu[1].toFixed(2)}, Z: {pos_enu[2].toFixed(2)}
                </span>
              )}
              {coordinateType === "gps" && cur_lat != null && cur_lon != null && (
                <span style={{ fontSize: "13px", color: "#1890ff", fontWeight: 500 }}>
                  当前位置 (GPS): 经度: {Number(cur_lon).toFixed(6)}, 纬度: {Number(cur_lat).toFixed(6)}
                </span>
              )}
            </div>
            <div className="flex flex-row gap-2">
              <Button size="small" onClick={undo} disabled={!canUndo}>
                撤销
              </Button>
              <Button size="small" onClick={redo} disabled={!canRedo}>
                重做
              </Button>
              <Button size="small" onClick={clearWaypoints}>
                清空
              </Button>
            </div>
          </div>

          <Table
            dataSource={displayWaypoints}
            columns={coordinateType === "gps" ? gpsColumns : enuColumns}
            rowKey="id"
            size="small"
            pagination={false}
            scroll={{ y: 200 }}
            rowClassName={(_, index) => {
              if (wp_idx !== undefined && wp_idx === index) {
                return "waypoint-highlight";
              }
              return "";
            }}
          />

          <div className="mt-3 flex-row flex justify-center">
            <Space>
              <Button
                type="primary"
                onClick={submitWaypoints}
                disabled={waypoints.length < 2}
              >
                提交航点
              </Button>
              {isEditing && (
                <Button onClick={handleCancelEdit}>取消编辑</Button>
              )}
            </Space>
          </div>
        </>
      )}

      {mode === "follow" && (
        <FollowControlPanel
          followState={followState}
          setFollowState={setFollowState}
          stopFollow={stopFollow}
        />
      )}
    </div>
  );
};
