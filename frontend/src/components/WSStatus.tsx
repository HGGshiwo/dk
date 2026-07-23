import { Tag } from 'antd';
import { useAppStore } from '../store/useAppStore';
import { useEffect, useState } from 'react';

export const WSStatus = () => {
  const wsStatus = useAppStore(state => state.wsStatus);
  const lastMessageTime = useAppStore(state => state.lastMessageTime);
  const [active, setActive] = useState(false);

  useEffect(() => {
    if (!lastMessageTime) return;
    setActive(true);
    const timer = setTimeout(() => {
      setActive(false);
    }, 150); // Flash duration
    return () => clearTimeout(timer);
  }, [lastMessageTime]);

  const getStatusTag = () => {
    switch (wsStatus) {
      case 'connecting': return { color: 'gold', text: 'WS连接中...' };
      case 'open': return { color: 'green', text: 'WS已连接' };
      case 'closed': return { color: 'red', text: 'WS已断开（自动重连中）' };
      case 'error': return { color: 'orange', text: 'WS连接错误' };
      default: return { color: 'gray', text: 'WS状态未知' };
    }
  };

  const { color, text } = getStatusTag();

  return (
    <div className="mb-2 flex items-center w-full">
      <Tag 
        color={color} 
        style={{
          fontSize: "16px", 
          padding: "6px 12px", 
          display: 'inline-flex', 
          alignItems: 'center', 
          gap: '8px'
        }}
      >
        <span>{text}</span>
        {wsStatus === 'open' && (
          <span 
            style={{
              width: '8px',
              height: '8px',
              borderRadius: '50%',
              backgroundColor: '#52c41a',
              transition: 'all 0.1s ease-in-out',
              transform: active ? 'scale(1.8)' : 'scale(1)',
              filter: active ? 'brightness(1.5) drop-shadow(0 0 6px #52c41a)' : 'none',
              display: 'inline-block'
            }}
          />
        )}
      </Tag>
    </div>
  );
};