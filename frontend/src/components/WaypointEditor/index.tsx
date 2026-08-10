import React, { useRef, useEffect, useState, useCallback, useMemo } from "react";
import { message } from "antd";
import "./WaypointEditor.css";
import { useAppStore } from "../../store/useAppStore";
import { httpRequest } from "../../utils";
import { useOrigin } from "./hooks";
import { VirtualJoystick } from "../VirtualJoystick";
import {
  type Point,
  type Waypoint,
  type ViewState,
  type FollowState,
  type WaypointEditorProps,
  DEFAULT_HEIGHT,
  DEFAULT_SCALE,
  FOLLOW_INTERVAL_MS,
  METERS_PER_DEGREE_LAT,
  METERS_PER_DEGREE_LON,
} from "./types";
import { useWaypointHistory } from "./hooks/useWaypointHistory";
import { useCanvasRender } from "./hooks/useCanvasRender";
import { useCanvasInteraction } from "./hooks/useCanvasInteraction";
import { WaypointToolbar } from "./components/WaypointToolbar";
import { WaypointTablePanel } from "./components/WaypointTablePanel";

export type { WaypointData, WaypointEditorProps } from "./types";

const WaypointEditor: React.FC<WaypointEditorProps> = ({
  waypointSubmitUrl = "/set_waypoint",
  followSubmitUrl = "/set_posvel",
  onEditModeChange,
}) => {
  const [mode, setMode] = useState<"waypoint" | "follow">("waypoint");
  const [showWaypointPanel, setShowWaypointPanel] = useState(false);
  const [showJoystick, setShowJoystick] = useState(false);
  const [coordinateType, setCoordinateType] = useState<"gps" | "local">("local");
  const [isEditing, setIsEditing] = useState(false);
  const [isLoading, setIsLoading] = useState<boolean>(true);
  const [waypoints, setWaypoints] = useState<Waypoint[]>([]);

  const [followState, setFollowState] = useState<FollowState>({
    isDrawing: false,
    startPoint: null,
    currentMousePoint: null,
    fixedHeading: false,
    followHeight: DEFAULT_HEIGHT,
    followSpeed: 2,
    isFollowing: false,
  });

  const joystickState = useRef({ x: 0, y: 0, z: 0, w: 0 });
  const canvasRef = useRef<HTMLCanvasElement>(null);
  const containerRef = useRef<HTMLDivElement>(null);
  const followIntervalRef = useRef<ReturnType<typeof setInterval> | null>(null);

  // 视口与显示原点
  const viewRef = useRef<ViewState>({
    offsetX: 0,
    offsetY: 0,
    scale: DEFAULT_SCALE,
  });

  // 画布十字中心对应的 ENU 绝对坐标 (默认为 0, 0，收到飞机 pos_enu 或点击重置时对齐飞机)
  const displayOriginRef = useRef<Point>({ x: 0, y: 0 });
  const hasInitializedOriginRef = useRef(false);

  const interactionRef = useRef<{
    type: "none" | "dragging-waypoint" | "panning";
    waypointId?: string;
    startOffset?: { dx: number; dy: number };
    startWorld?: Point;
    startCanvas?: { x: number; y: number };
  }>({ type: "none" });

  // Store 状态订阅
  const mission_data = useAppStore((state) => state.stateData.mission_data);
  const wp_idx = useAppStore((state) => state.stateData.wp_idx) as number | undefined;
  const pos_enu = useAppStore((state) => state.stateData.pos_enu) as number[] | undefined;
  const cur_lat = useAppStore((state) => state.stateData.lat) as number | undefined;
  const cur_lon = useAppStore((state) => state.stateData.lon) as number | undefined;

  // 历史栈管理
  const { pushToHistory, undo, redo, resetHistory, canUndo, canRedo } =
    useWaypointHistory(setWaypoints);

  // GPS 原点 (用于辅助经纬度与 ENU 之间的转换)
  const originRef = useOrigin(() => {
    setIsLoading(false);
  });

  // 初始化 ENU 原点到飞机位置
  useEffect(() => {
    if (!hasInitializedOriginRef.current && pos_enu && pos_enu.length >= 2) {
      displayOriginRef.current = { x: pos_enu[0], y: pos_enu[1] };
      hasInitializedOriginRef.current = true;
      setIsLoading(false);
    }
  }, [pos_enu]);

  // GPS 坐标转换
  const gpsToEnu = useCallback(
    (lat: number, lon: number): Point => {
      const origin = originRef.current;
      if (!origin || (origin.lat === 0 && origin.lon === 0)) {
        return { x: 0, y: 0 };
      }
      return {
        x: (lon - origin.lon) * METERS_PER_DEGREE_LON(origin.lat),
        y: (lat - origin.lat) * METERS_PER_DEGREE_LAT,
      };
    },
    [originRef]
  );

  const enuToGps = useCallback(
    (x: number, y: number): { lat: number; lon: number } => {
      const origin = originRef.current;
      if (!origin || (origin.lat === 0 && origin.lon === 0)) {
        return { lat: 0, lon: 0 };
      }
      return {
        lon: origin.lon + x / METERS_PER_DEGREE_LON(origin.lat),
        lat: origin.lat + y / METERS_PER_DEGREE_LAT,
      };
    },
    [originRef]
  );

  // 后端 mission_data 始终是 ENU 坐标
  const backendWaypoints = useMemo<Waypoint[]>(() => {
    return ((mission_data as any[]) || []).map((wp, idx) => {
      const x = wp.enu_x ?? 0;
      const y = wp.enu_y ?? 0;
      const z = wp.enu_z ?? wp.alt ?? wp.height ?? 0;
      const gps = enuToGps(x, y);

      return {
        id: `backend-wp-${idx}`,
        x,
        y,
        z,
        lon: gps.lon,
        lat: gps.lat,
        alt: z,
      };
    });
  }, [mission_data, enuToGps]);

  const displayWaypoints = isEditing ? waypoints : backendWaypoints;

  // 非编辑状态下，与后端的 mission_data 同步
  useEffect(() => {
    if (!isEditing) {
      const newWaypoints = ((mission_data as any[]) || []).map((wp, idx) => {
        const x = wp.enu_x ?? 0;
        const y = wp.enu_y ?? 0;
        const z = wp.enu_z ?? wp.alt ?? wp.height ?? 0;
        const gps = enuToGps(x, y);

        return {
          id: `wp-${Date.now()}-${idx}-${Math.random()}`,
          x,
          y,
          z,
          lon: gps.lon,
          lat: gps.lat,
          alt: z,
        };
      });
      setWaypoints(newWaypoints);
      resetHistory(newWaypoints);
    }
  }, [mission_data, isEditing, enuToGps, resetHistory]);

  // 渲染循环
  const { worldToCanvas, canvasToWorld, requestRedraw } = useCanvasRender({
    canvasRef,
    viewRef,
    displayOriginRef,
    mode,
    displayWaypoints,
    followState,
    interactionRef,
    isLoading,
  });

  // 交互处理
  const {
    handleMouseDown,
    handleMouseMove,
    handleMouseUp,
    handleMouseLeave,
    handleContextMenu,
  } = useCanvasInteraction({
    canvasRef,
    viewRef,
    displayOriginRef,
    interactionRef,
    mode,
    showWaypointPanel,
    waypoints,
    setWaypoints,
    isEditing,
    setIsEditing,
    pushToHistory,
    followState,
    setFollowState,
    enuToGps,
    worldToCanvas,
    canvasToWorld,
    requestRedraw,
  });

  // 摇杆指令下发
  useEffect(() => {
    httpRequest("POST", "/joystick/enable", { enable: showJoystick });

    let interval: ReturnType<typeof setInterval>;
    if (showJoystick) {
      interval = setInterval(() => {
        httpRequest("POST", "/cmd_vel", {
          x: joystickState.current.x,
          y: joystickState.current.y,
          z: joystickState.current.z,
          w: joystickState.current.w,
        });
      }, 100);
    }
    return () => {
      if (interval) clearInterval(interval);
    };
  }, [showJoystick]);

  useEffect(() => {
    onEditModeChange?.(showWaypointPanel || showJoystick);
  }, [showWaypointPanel, showJoystick, onEditModeChange]);

  useEffect(() => {
    if (!showWaypointPanel) {
      setIsEditing(false);
    }
  }, [showWaypointPanel]);

  const stopFollow = useCallback(() => {
    setFollowState((prev) => ({
      ...prev,
      isFollowing: false,
      startPoint: null,
      currentMousePoint: null,
    }));
  }, []);

  const handleModeChange = useCallback(
    (newMode: "waypoint" | "follow") => {
      setMode(newMode);
      if (newMode === "follow") {
        setFollowState((prev) => ({
          ...prev,
          isDrawing: false,
          startPoint: null,
          currentMousePoint: null,
          fixedHeading: false,
          followHeight: DEFAULT_HEIGHT,
          isFollowing: false,
        }));
      } else {
        stopFollow();
      }
    },
    [stopFollow]
  );

  // 跟随模式周期下发
  useEffect(() => {
    if (mode === "follow" && followState.isFollowing && followState.startPoint) {
      if (followIntervalRef.current) clearInterval(followIntervalRef.current);
      followIntervalRef.current = setInterval(() => {
        const origin = originRef.current;
        if (!origin) return;
        const direction = followState.currentMousePoint
          ? {
              dx: followState.currentMousePoint.x - followState.startPoint!.x,
              dy: followState.currentMousePoint.y - followState.startPoint!.y,
            }
          : { dx: 0, dy: 0 };
        const deltaLon = direction.dx / METERS_PER_DEGREE_LON(origin.lat);
        const deltaLat = direction.dy / METERS_PER_DEGREE_LAT;
        const yaw = Math.atan2(deltaLon, deltaLat);
        const data = {
          pos: [
            followState.startPoint!.x / METERS_PER_DEGREE_LON(origin.lat) + origin.lon,
            followState.startPoint!.y / METERS_PER_DEGREE_LAT + origin.lat,
            followState.followHeight,
          ],
          yaw,
          fix_yaw: followState.fixedHeading,
          vel: followState.followSpeed,
        };
        httpRequest("POST", followSubmitUrl, data);
      }, FOLLOW_INTERVAL_MS);
    } else {
      if (followIntervalRef.current) {
        clearInterval(followIntervalRef.current);
        followIntervalRef.current = null;
      }
    }
    return () => {
      if (followIntervalRef.current) clearInterval(followIntervalRef.current);
    };
  }, [
    mode,
    followState.isFollowing,
    followState.startPoint,
    followState.currentMousePoint,
    followState.fixedHeading,
    followState.followHeight,
    followSubmitUrl,
    originRef,
  ]);

  // 重置：将展示十字中心对准飞机当前的 ENU 坐标位置，并恢复默认缩放与居中视口
  const handleReset = useCallback(() => {
    const curPosEnu = useAppStore.getState().stateData.pos_enu as number[] | undefined;
    if (curPosEnu && curPosEnu.length >= 2) {
      displayOriginRef.current = {
        x: curPosEnu[0],
        y: curPosEnu[1],
      };
    }
    viewRef.current = {
      offsetX: 0,
      offsetY: 0,
      scale: DEFAULT_SCALE,
    };
    requestRedraw();
  }, [requestRedraw]);

  // 提交航点 (发送绝对坐标)
  const submitWaypoints = useCallback(() => {
    if (waypoints.length === 0) {
      message.warning("没有航点可提交");
      return;
    }

    const isLocal = coordinateType === "local";
    const data = isLocal
      ? waypoints.map((wp) => [wp.x, wp.y, wp.z])
      : waypoints.map((wp) => [wp.lon, wp.lat, wp.alt]);

    httpRequest("POST", waypointSubmitUrl, {
      waypoint: data,
      local: isLocal,
    })
      .then(() => {
        message.success("航点提交成功");
        setIsEditing(false);
        setShowWaypointPanel(false);
      })
      .catch(() => message.error("航点提交失败"));
  }, [waypoints, waypointSubmitUrl, coordinateType]);

  const clearWaypoints = useCallback(() => {
    setIsEditing(true);
    pushToHistory([]);
    setWaypoints([]);
  }, [pushToHistory]);

  const handleCancelEdit = useCallback(() => {
    setIsEditing(false);
    setShowWaypointPanel(false);
  }, []);

  return (
    <div className="waypoint-editor-fullscreen" ref={containerRef}>
      <div className="canvas-container-fullscreen">
        <canvas
          ref={canvasRef}
          className="canvas"
          style={{ cursor: showWaypointPanel ? "crosshair" : "grab" }}
          onMouseDown={handleMouseDown}
          onMouseMove={handleMouseMove}
          onMouseUp={handleMouseUp}
          onMouseLeave={handleMouseLeave}
          onContextMenu={handleContextMenu}
        />

        {/* 右侧悬浮按钮组 */}
        <WaypointToolbar
          showWaypointPanel={showWaypointPanel}
          setShowWaypointPanel={setShowWaypointPanel}
          showJoystick={showJoystick}
          setShowJoystick={setShowJoystick}
          onReset={handleReset}
        />
      </div>

      {/* 虚拟摇杆 */}
      {showJoystick && (
        <div
          style={{
            position: "absolute",
            bottom: "120px",
            left: "40px",
            right: "40px",
            display: "flex",
            justifyContent: "space-between",
            zIndex: 50,
            pointerEvents: "none",
          }}
        >
          <div style={{ pointerEvents: "auto" }}>
            <VirtualJoystick
              onMove={(data) => {
                joystickState.current.z = data.y;
                joystickState.current.w = data.x;
              }}
            />
          </div>
          <div style={{ pointerEvents: "auto" }}>
            <VirtualJoystick
              onMove={(data) => {
                joystickState.current.x = data.y;
                joystickState.current.y = data.x;
              }}
            />
          </div>
        </div>
      )}

      {/* 底部航点编辑面板 */}
      {showWaypointPanel && (
        <WaypointTablePanel
          mode={mode}
          handleModeChange={handleModeChange}
          coordinateType={coordinateType}
          setCoordinateType={setCoordinateType}
          displayWaypoints={displayWaypoints}
          waypoints={waypoints}
          setWaypoints={setWaypoints}
          isEditing={isEditing}
          setIsEditing={setIsEditing}
          pushToHistory={pushToHistory}
          undo={undo}
          redo={redo}
          canUndo={canUndo}
          canRedo={canRedo}
          clearWaypoints={clearWaypoints}
          submitWaypoints={submitWaypoints}
          handleCancelEdit={handleCancelEdit}
          enuToGps={enuToGps}
          gpsToEnu={gpsToEnu}
          wp_idx={wp_idx}
          pos_enu={pos_enu}
          cur_lat={cur_lat}
          cur_lon={cur_lon}
          followState={followState}
          setFollowState={setFollowState}
          stopFollow={stopFollow}
        />
      )}
    </div>
  );
};

export default WaypointEditor;
