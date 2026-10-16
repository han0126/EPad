#pragma once

class WebServer;

namespace calendar_events_portal {

// 访问权限、热点倒计时和忙碌状态由现有配网模块统一判断。
void registerRoutes(WebServer& server, bool (*readAllowed)(), bool (*writeAllowed)());

}  // namespace calendar_events_portal
