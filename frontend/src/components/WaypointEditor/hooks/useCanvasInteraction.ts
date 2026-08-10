import { useCallback, useEffect } from "react";
import { useAppStore } from "../../../store/useAppStore";
import {
  type Point,
  type Waypoint,
  type ViewState,
  type FollowState,
  HIT_TOLERANCE,
  DRAG_THRESHOLD,
  DEFAULT_HEIGHT,
} from "../types";

interface UseCanvasInteractionProps {
  canvasRef: React.RefObject<HTMLCanvasElement | null>;
  viewRef: React.RefObject<ViewState>;
  displayOriginRef: React.RefObject<Point>;
  interactionRef: React.MutableRefObject<{
    type: "none" | "dragging-waypoint" | "panning";
    waypointId?: string;
    startOffset?: { dx: number; dy: number };
    startWorld?: Point;
    startCanvas?: { x: number; y: number };
  }>;
  mode: "waypoint" | "follow";
  showWaypointPanel: boolean;
  waypoints: Waypoint[];
  setWaypoints: React.Dispatch<React.SetStateAction<Waypoint[]>>;
  isEditing: boolean;
  setIsEditing: (editing: boolean) => void;
  pushToHistory: (waypoints: Waypoint[]) => void;
  followState: FollowState;
  setFollowState: React.Dispatch<React.SetStateAction<FollowState>>;
  enuToGps: (x: number, y: number) => { lat: number; lon: number };
  worldToCanvas: (worldX: number, worldY: number) => { canvasX: number; canvasY: number };
  canvasToWorld: (canvasX: number, canvasY: number) => Point;
  requestRedraw: () => void;
}

export const useCanvasInteraction = ({
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
}: UseCanvasInteractionProps) => {
  const handleMouseDown = useCallback(
    (e: React.MouseEvent<HTMLCanvasElement>) => {
      e.preventDefault();
      if (!canvasRef.current) return;

      const rect = canvasRef.current.getBoundingClientRect();
      const canvasX = e.clientX - rect.left;
      const canvasY = e.clientY - rect.top;
      const worldPos = canvasToWorld(canvasX, canvasY);

      if (mode === "waypoint") {
        if (e.button === 0) {
          const hit = showWaypointPanel
            ? waypoints.find((wp) => {
                const { canvasX: wpX, canvasY: wpY } = worldToCanvas(wp.x, wp.y);
                return Math.hypot(canvasX - wpX, canvasY - wpY) < HIT_TOLERANCE;
              })
            : null;

          if (hit) {
            if (!isEditing) setIsEditing(true);
            pushToHistory(waypoints);
            interactionRef.current = {
              type: "dragging-waypoint",
              waypointId: hit.id,
              startWorld: worldPos,
              startCanvas: { x: canvasX, y: canvasY },
              startOffset: {
                dx: hit.x - worldPos.x,
                dy: hit.y - worldPos.y,
              },
            };
          } else {
            interactionRef.current = {
              type: "none",
              startWorld: worldPos,
              startCanvas: { x: canvasX, y: canvasY },
            };
          }
        } else if (e.button === 2) {
          interactionRef.current = {
            type: "panning",
            startWorld: worldPos,
            startCanvas: { x: canvasX, y: canvasY },
          };
        }
      } else if (mode === "follow") {
        if (e.button === 0) {
          setFollowState((prev) => ({
            ...prev,
            isDrawing: true,
            startPoint: worldPos,
            currentMousePoint: worldPos,
          }));
        } else if (e.button === 2) {
          interactionRef.current = {
            type: "panning",
            startWorld: worldPos,
            startCanvas: { x: canvasX, y: canvasY },
          };
        }
      }
    },
    [
      canvasRef,
      canvasToWorld,
      mode,
      showWaypointPanel,
      waypoints,
      worldToCanvas,
      isEditing,
      setIsEditing,
      pushToHistory,
      interactionRef,
      setFollowState,
    ]
  );

  const handleMouseMove = useCallback(
    (e: React.MouseEvent<HTMLCanvasElement>) => {
      if (!canvasRef.current) return;
      const rect = canvasRef.current.getBoundingClientRect();
      const canvasX = e.clientX - rect.left;
      const canvasY = e.clientY - rect.top;
      const worldPos = canvasToWorld(canvasX, canvasY);

      const interaction = interactionRef.current;

      if (interaction.type === "panning" && interaction.startCanvas) {
        const dx = canvasX - interaction.startCanvas.x;
        const dy = canvasY - interaction.startCanvas.y;
        viewRef.current = {
          ...viewRef.current,
          offsetX: viewRef.current.offsetX + dx,
          offsetY: viewRef.current.offsetY + dy,
        };
        interactionRef.current = {
          ...interaction,
          startCanvas: { x: canvasX, y: canvasY },
        };
        requestRedraw();
        return;
      }

      if (mode === "waypoint") {
        if (
          interaction.type === "dragging-waypoint" &&
          interaction.waypointId &&
          interaction.startOffset
        ) {
          if (!isEditing) {
            setIsEditing(true);
          }
          if (waypoints.findIndex((v) => v.id === interaction.waypointId) === 0) {
            return;
          }
          const newWorldX = worldPos.x + interaction.startOffset.dx;
          const newWorldY = worldPos.y + interaction.startOffset.dy;
          const newGps = enuToGps(newWorldX, newWorldY);

          setWaypoints((prev) =>
            prev.map((wp) =>
              wp.id === interaction.waypointId
                ? {
                    ...wp,
                    x: newWorldX,
                    y: newWorldY,
                    lon: newGps.lon,
                    lat: newGps.lat,
                  }
                : wp
            )
          );
          return;
        }

        if (
          interaction.type === "none" &&
          interaction.startCanvas &&
          e.buttons === 1
        ) {
          const dist = Math.hypot(
            canvasX - interaction.startCanvas.x,
            canvasY - interaction.startCanvas.y
          );
          if (dist > DRAG_THRESHOLD) {
            interactionRef.current = {
              type: "panning",
              startWorld: interaction.startWorld,
              startCanvas: interaction.startCanvas,
            };
          }
        }
      } else if (mode === "follow") {
        if (followState.isDrawing) {
          setFollowState((prev) => ({ ...prev, currentMousePoint: worldPos }));
        }
      }
    },
    [
      canvasRef,
      canvasToWorld,
      interactionRef,
      mode,
      requestRedraw,
      viewRef,
      isEditing,
      setIsEditing,
      waypoints,
      enuToGps,
      setWaypoints,
      followState.isDrawing,
      setFollowState,
    ]
  );

  const handleMouseUp = useCallback(
    (e: React.MouseEvent<HTMLCanvasElement>) => {
      if (!canvasRef.current) return;

      const rect = canvasRef.current.getBoundingClientRect();
      const canvasX = e.clientX - rect.left;
      const canvasY = e.clientY - rect.top;

      if (mode === "waypoint") {
        const interaction = interactionRef.current;
        if (interaction.type === "dragging-waypoint") {
          pushToHistory(waypoints);
        }

        if (
          e.button === 0 &&
          interaction.type === "none" &&
          interaction.startCanvas &&
          showWaypointPanel
        ) {
          if (!isEditing) setIsEditing(true);
          const dist = Math.hypot(
            canvasX - interaction.startCanvas.x,
            canvasY - interaction.startCanvas.y
          );

          if (dist <= DRAG_THRESHOLD) {
            const worldPos = canvasToWorld(canvasX, canvasY);
            setWaypoints((prev) => {
              let curWP = [...prev];
              const createWp = (x: number, y: number, z: number = DEFAULT_HEIGHT): Waypoint => {
                const gps = enuToGps(x, y);
                return {
                  id: `wp-${Date.now()}-${Math.random()}`,
                  x,
                  y,
                  z,
                  lon: gps.lon,
                  lat: gps.lat,
                  alt: z,
                };
              };

              if (prev.length === 0) {
                // 若无航点，以机器人当前位置作为第一点（起飞点/初始点）
                const curPosEnu = useAppStore.getState().stateData.pos_enu as number[] | undefined;
                const initX = curPosEnu && curPosEnu.length >= 2 ? curPosEnu[0] : displayOriginRef.current.x;
                const initY = curPosEnu && curPosEnu.length >= 2 ? curPosEnu[1] : displayOriginRef.current.y;
                curWP = [createWp(initX, initY)];
              }

              const newWaypoints = [...curWP, createWp(worldPos.x, worldPos.y)];
              pushToHistory(newWaypoints);
              return newWaypoints;
            });
          }
        }
        interactionRef.current = { type: "none" };
      } else if (mode === "follow") {
        if (followState.isDrawing && e.button === 0) {
          setFollowState((prev) => ({
            ...prev,
            isDrawing: false,
            isFollowing: true,
          }));
        }
        if (e.button === 2) {
          interactionRef.current = { type: "none" };
        }
      }
    },
    [
      canvasRef,
      mode,
      interactionRef,
      showWaypointPanel,
      isEditing,
      setIsEditing,
      canvasToWorld,
      setWaypoints,
      pushToHistory,
      waypoints,
      enuToGps,
      displayOriginRef,
      followState.isDrawing,
      setFollowState,
    ]
  );

  const handleMouseLeave = useCallback(() => {
    if (mode === "follow" && followState.isDrawing) {
      setFollowState((prev) => ({ ...prev, isDrawing: false }));
    }
    interactionRef.current = { type: "none" };
  }, [mode, followState.isDrawing, setFollowState, interactionRef]);

  useEffect(() => {
    const handleWheel = (e: WheelEvent) => {
      e.preventDefault();
      e.stopPropagation();
      const rect = canvasRef.current?.getBoundingClientRect();
      if (!rect) return;
      const mouseCanvasX = e.clientX - rect.left;
      const mouseCanvasY = e.clientY - rect.top;
      const worldPos = canvasToWorld(mouseCanvasX, mouseCanvasY);

      const delta = e.deltaY > 0 ? 0.9 : 1.1;
      const newScale = Math.max(1, Math.min(100, viewRef.current.scale * delta));

      viewRef.current = {
        scale: newScale,
        offsetX:
          viewRef.current.offsetX -
          (worldPos.x - displayOriginRef.current.x) * (newScale - viewRef.current.scale),
        offsetY:
          viewRef.current.offsetY +
          (worldPos.y - displayOriginRef.current.y) * (newScale - viewRef.current.scale),
      };
      requestRedraw();
    };

    const canvas = canvasRef.current;
    canvas?.addEventListener("wheel", handleWheel, { passive: false });
    return () => {
      canvas?.removeEventListener("wheel", handleWheel);
    };
  }, [canvasRef, canvasToWorld, displayOriginRef, requestRedraw, viewRef]);

  const handleContextMenu = useCallback((e: React.MouseEvent) => {
    e.preventDefault();
  }, []);

  return {
    handleMouseDown,
    handleMouseMove,
    handleMouseUp,
    handleMouseLeave,
    handleContextMenu,
  };
};
