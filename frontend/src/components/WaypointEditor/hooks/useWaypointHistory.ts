import { useRef, useCallback } from "react";
import type { Waypoint } from "../types";

export const useWaypointHistory = (
  setWaypoints: React.Dispatch<React.SetStateAction<Waypoint[]>>
) => {
  const historyRef = useRef<{
    undoStack: Waypoint[][];
    redoStack: Waypoint[][];
  }>({ undoStack: [], redoStack: [] });

  const pushToHistory = useCallback((newWaypoints: Waypoint[]) => {
    historyRef.current.undoStack.push(newWaypoints);
    historyRef.current.redoStack = [];
  }, []);

  const undo = useCallback(() => {
    const { undoStack, redoStack } = historyRef.current;
    if (undoStack.length <= 1) return;
    const current = undoStack.pop()!;
    redoStack.push(current);
    const previous = undoStack[undoStack.length - 1];
    setWaypoints(previous || []);
  }, [setWaypoints]);

  const redo = useCallback(() => {
    const { undoStack, redoStack } = historyRef.current;
    if (redoStack.length === 0) return;
    const next = redoStack.pop()!;
    undoStack.push(next);
    setWaypoints(next);
  }, [setWaypoints]);

  const resetHistory = useCallback((initialWaypoints: Waypoint[]) => {
    historyRef.current = {
      undoStack: [initialWaypoints],
      redoStack: [],
    };
  }, []);

  const canUndo = historyRef.current.undoStack.length > 1;
  const canRedo = historyRef.current.redoStack.length > 0;

  return {
    historyRef,
    pushToHistory,
    undo,
    redo,
    resetHistory,
    canUndo,
    canRedo,
  };
};
