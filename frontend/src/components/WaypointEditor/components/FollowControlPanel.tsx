import React from "react";
import { Button, InputNumber, Switch } from "antd";
import type { FollowState } from "../types";

interface FollowControlPanelProps {
  followState: FollowState;
  setFollowState: React.Dispatch<React.SetStateAction<FollowState>>;
  stopFollow: () => void;
}

export const FollowControlPanel: React.FC<FollowControlPanelProps> = ({
  followState,
  setFollowState,
  stopFollow,
}) => {
  return (
    <div className="follow-panel">
      <div className="follow-row">
        <span>固定航向</span>
        <Switch
          checked={followState.fixedHeading}
          onChange={(checked) =>
            setFollowState((prev) => ({
              ...prev,
              fixedHeading: checked,
            }))
          }
        />
      </div>
      <div className="follow-row">
        <span>跟随高度</span>
        <InputNumber
          mode="spinner"
          value={followState.followHeight}
          onChange={(val) =>
            setFollowState((prev) => ({
              ...prev,
              followHeight: val || 0,
            }))
          }
          min={0}
          step={1}
        />
      </div>
      <div className="follow-row">
        <span>跟随速度</span>
        <InputNumber
          mode="spinner"
          value={followState.followSpeed}
          onChange={(val) =>
            setFollowState((prev) => ({
              ...prev,
              followSpeed: val || 0,
            }))
          }
          min={0}
          step={1}
        />
      </div>
      <div className="follow-row">
        <Button
          type="primary"
          danger={followState.isFollowing}
          onClick={stopFollow}
          disabled={!followState.isFollowing}
        >
          停止跟随
        </Button>
      </div>
      {followState.isFollowing && (
        <div className="follow-status">
          跟随中... 方向:{" "}
          {followState.currentMousePoint
            ? `(${(
                followState.currentMousePoint.x - (followState.startPoint?.x || 0)
              ).toFixed(2)}, ${(
                followState.currentMousePoint.y - (followState.startPoint?.y || 0)
              ).toFixed(2)})`
            : "无"}
        </div>
      )}
    </div>
  );
};
