#include "stdafx.h"
#include "LuaApi.h"
#include "Lua/lua.hpp"
#include "Debugger/LuaCallHelper.h"
#include "Debugger/Debugger.h"
#include "Debugger/MemoryDumper.h"
#include "Debugger/ScriptingContext.h"
#include "Debugger/MemoryAccessCounter.h"
#include "Debugger/LabelManager.h"
#include "Shared/Video/DebugHud.h"
#include "Shared/Video/VideoDecoder.h"
#include "Shared/MessageManager.h"
#include "Shared/RewindManager.h"
#include "Shared/SaveStateManager.h"
#include "Shared/Emulator.h"
#include "Shared/Video/BaseVideoFilter.h"
#include "Shared/Video/DrawScreenBufferCommand.h"
#include "Shared/Video/DrawStringCommand.h"
#include "Shared/KeyManager.h"
#include "Shared/Interfaces/IConsole.h"
#include "Shared/Interfaces/IKeyManager.h"
#include "Shared/ControllerHub.h"
#include "Shared/BaseControlManager.h"
#include "Utilities/HexUtilities.h"
#include "Utilities/FolderUtilities.h"
#include "Utilities/magic_enum.hpp"
#include "MemoryOperationType.h"

#ifdef _MSC_VER
#pragma warning( disable : 4702 ) //unreachable code
#endif

#define lua_pushintvalue(name, value) lua_pushliteral(lua, #name); lua_pushinteger(lua, (int)value); lua_settable(lua, -3);
#define lua_pushdoublevalue(name, value) lua_pushliteral(lua, #name); lua_pushnumber(lua, (double)value); lua_settable(lua, -3);
#define lua_pushboolvalue(name, value) lua_pushliteral(lua, #name); lua_pushboolean(lua, (int)value); lua_settable(lua, -3);
#define lua_pushstringvalue(name, value) lua_pushliteral(lua, #name); lua_pushstring(lua, value.c_str()); lua_settable(lua, -3);
#define lua_pusharrayvalue(index, value) lua_pushinteger(lua, index); lua_pushinteger(lua, value); lua_settable(lua, -3);

#define lua_starttable(name) lua_pushliteral(lua, name); lua_newtable(lua);
#define lua_endtable() lua_settable(lua, -3);
#define lua_readint(name, dest) lua_getfield(lua, -1, #name); dest = l.ReadInteger();
#define lua_readbool(name, dest) lua_getfield(lua, -1, #name); dest = l.ReadBool();
#define error(text) luaL_error(lua, text); return 0;
#define errorCond(cond, text) if(cond) { luaL_error(lua, text); return 0; }
#define checkEnum(enumType, enumValue, text) if(!magic_enum::enum_contains<enumType>(enumValue)) { luaL_error(lua, text); return 0; }

#define checkparams() if(!l.CheckParamCount()) { return 0; }
#define checkminparams(x) if(!l.CheckParamCount(x)) { return 0; }
#define checkinitdone() if(!_context->CheckInitDone()) { error("This function cannot be called outside a callback"); }
#define checksavestateconditions() if(!_context->CheckInStartFrameEvent() && !_context->CheckInExecOpEvent()) { error("This function must be called inside a StartFrame event callback or a CpuExec memory operation callback"); }

Debugger* LuaApi::_debugger = nullptr;
Emulator* LuaApi::_emu = nullptr;
MemoryDumper* LuaApi::_memoryDumper = nullptr;
ScriptingContext* LuaApi::_context = nullptr;

void LuaApi::SetContext(ScriptingContext* context)
{
	_context = context;
	_debugger = _context->GetDebugger();
	_memoryDumper = _debugger->GetMemoryDumper();
	_emu = _debugger->GetEmulator();
}

void LuaApi::LuaPushIntValue(lua_State* lua, string name, int value)
{
	lua_pushstring(lua, name.c_str());
	lua_pushinteger(lua, value);
	lua_settable(lua, -3);
}

int LuaApi::GetLibrary(lua_State *lua)
{
	static const luaL_Reg apilib[] = {
		{ "read", LuaApi::ReadMemory },
		{ "write", LuaApi::WriteMemory },
		{ "readWord", LuaApi::ReadMemoryWord },
		{ "writeWord", LuaApi::WriteMemoryWord },
		
		{ "convertAddress", LuaApi::ConvertAddress },
		{ "getLabelAddress", LuaApi::GetLabelAddress },

		{ "addMemoryCallback", LuaApi::RegisterMemoryCallback },
		{ "removeMemoryCallback", LuaApi::UnregisterMemoryCallback },
		{ "addEventCallback", LuaApi::RegisterEventCallback },
		{ "removeEventCallback", LuaApi::UnregisterEventCallback },

		{ "measureString", LuaApi::MeasureString },
		{ "drawString", LuaApi::DrawString },

		{ "drawPixel", LuaApi::DrawPixel },
		{ "drawLine", LuaApi::DrawLine },
		{ "drawRectangle", LuaApi::DrawRectangle },
		{ "clearScreen", LuaApi::ClearScreen },		

		{ "getScreenSize", LuaApi::GetScreenSize },
		{ "getScreenBuffer", LuaApi::GetScreenBuffer },
		{ "setScreenBuffer", LuaApi::SetScreenBuffer },
		{ "getPixel", LuaApi::GetPixel },

		{ "getMouseState", LuaApi::GetMouseState },
		{ "log", LuaApi::Log },
		{ "displayMessage", LuaApi::DisplayMessage },

		{ "reset", LuaApi::Reset },
		{ "breakExecution", LuaApi::Break },
		{ "resume", LuaApi::Resume },
		{ "execute", LuaApi::Execute },
		{ "rewind", LuaApi::Rewind },

		{ "takeScreenshot", LuaApi::TakeScreenshot },

		{ "isKeyPressed", LuaApi::IsKeyPressed },
		{ "getInput", LuaApi::GetInput },
		{ "setInput", LuaApi::SetInput },

		{ "getAccessCounters", LuaApi::GetAccessCounters },
		{ "resetAccessCounters", LuaApi::ResetAccessCounters },
		
		{ "getState", LuaApi::GetState },
		{ "setState", LuaApi::SetState },

		{ "getScriptDataFolder", LuaApi::GetScriptDataFolder },
		{ "getRomInfo", LuaApi::GetRomInfo },
		{ "getLogWindowLog", LuaApi::GetLogWindowLog },
		{ NULL,NULL }
	};

	luaL_newlib(lua, apilib);

	//Expose MemoryType enum as "emu.memType"
	lua_pushliteral(lua, "memType");
	lua_newtable(lua);
	for(auto& entry : magic_enum::enum_entries<MemoryType>()) {
		string name = string(entry.second);
		name[0] = ::tolower(name[0]);
		if(DebugUtilities::IsRelativeMemory(entry.first)) {
			name = name.substr(0, name.size() - 6);
			string debugName = name + "Debug";
			LuaPushIntValue(lua, debugName, (int)entry.first | 0x100);
		}
		LuaPushIntValue(lua, name, (int)entry.first);
	}
	lua_settable(lua, -3);

	lua_pushliteral(lua, "memCallbackType");
	lua_newtable(lua);
	lua_pushintvalue(read, CallbackType::Read);
	lua_pushintvalue(write, CallbackType::Write);
	lua_pushintvalue(exec, CallbackType::Exec);
	lua_settable(lua, -3);

	lua_pushliteral(lua, "counterMemType");
	lua_newtable(lua);
	for(auto& entry : magic_enum::enum_entries<MemoryType>()) {
		string name = string(entry.second);
		name[0] = ::tolower(name[0]);
		if(!DebugUtilities::IsRelativeMemory(entry.first)) {
			LuaPushIntValue(lua, name, (int)entry.first);
		}
	}
	lua_settable(lua, -3);

	lua_pushliteral(lua, "counterOpType");
	lua_newtable(lua);
	lua_pushintvalue(read, MemoryOperationType::Read);
	lua_pushintvalue(write, MemoryOperationType::Write);
	lua_pushintvalue(exec, MemoryOperationType::ExecOpCode);
	lua_settable(lua, -3);

	lua_pushliteral(lua, "eventType");
	lua_newtable(lua);
	lua_pushintvalue(reset, EventType::Reset);
	lua_pushintvalue(nmi, EventType::Nmi);
	lua_pushintvalue(irq, EventType::Irq);
	lua_pushintvalue(startFrame, EventType::StartFrame);
	lua_pushintvalue(endFrame, EventType::EndFrame);
	lua_pushintvalue(inputPolled, EventType::InputPolled);
	lua_pushintvalue(scriptEnded, EventType::ScriptEnded);
	lua_pushintvalue(stateLoaded, EventType::StateLoaded);
	lua_pushintvalue(stateSaved, EventType::StateSaved);
	lua_pushintvalue(gbStartFrame, EventType::GbStartFrame);
	lua_pushintvalue(gbEndFrame, EventType::GbEndFrame);
	//TODO
	/*lua_pushintvalue(codeBreak, EventType::CodeBreak);
	*/
	lua_settable(lua, -3);

	lua_pushliteral(lua, "stepType");
	lua_newtable(lua);
	lua_pushintvalue(cpuInstructions, StepType::Step);
	lua_pushintvalue(ppuCycles, StepType::PpuStep);
	lua_settable(lua, -3);

	lua_pushliteral(lua, "cpuType");
	lua_newtable(lua);
	for(auto& entry : magic_enum::enum_entries<CpuType>()) {
		string name = string(entry.second);
		name[0] = ::tolower(name[0]);
		LuaPushIntValue(lua, name, (int)entry.first);
	}
	lua_settable(lua, -3);

	return 1;
}

int LuaApi::ReadMemory(lua_State *lua)
{
	LuaCallHelper l(lua);
	l.ForceParamCount(3);
	bool returnSignedValue = l.ReadBool();
	int type = l.ReadInteger();
	bool disableSideEffects = (type & 0x100) == 0x100;
	MemoryType memType = (MemoryType)(type & 0xFF);
	int address = l.ReadInteger();
	checkminparams(2);
	errorCond(address < 0, "address must be >= 0");
	uint8_t value = _memoryDumper->GetMemoryValue(memType, address, disableSideEffects);
	l.Return(returnSignedValue ? (int8_t)value : value);
	return l.ReturnCount();
}

int LuaApi::WriteMemory(lua_State *lua)
{
	LuaCallHelper l(lua);
	int type = l.ReadInteger();
	bool disableSideEffects = (type & 0x100) == 0x100;
	MemoryType memType = (MemoryType)(type & 0xFF);
	int value = l.ReadInteger();
	int address = l.ReadInteger();
	checkparams();
	errorCond(value > 255 || value < -128, "value out of range");
	errorCond(address < 0, "address must be >= 0");
	_memoryDumper->SetMemoryValue(memType, address, value, disableSideEffects);
	return l.ReturnCount();
}

int LuaApi::ReadMemoryWord(lua_State *lua)
{
	LuaCallHelper l(lua);
	l.ForceParamCount(3);
	bool returnSignedValue = l.ReadBool();
	int type = l.ReadInteger();
	bool disableSideEffects = (type & 0x100) == 0x100;
	MemoryType memType = (MemoryType)(type & 0xFF);
	int address = l.ReadInteger();
	checkminparams(2);
	errorCond(address < 0, "address must be >= 0");
	uint16_t value = _memoryDumper->GetMemoryValueWord(memType, address, disableSideEffects);
	l.Return(returnSignedValue ? (int16_t)value : value);
	return l.ReturnCount();
}

int LuaApi::WriteMemoryWord(lua_State *lua)
{
	LuaCallHelper l(lua);
	int type = l.ReadInteger();
	bool disableSideEffects = (type & 0x100) == 0x100;
	MemoryType memType = (MemoryType)(type & 0xFF);
	int value = l.ReadInteger();
	int address = l.ReadInteger();
	checkparams();
	errorCond(value > 65535 || value < -32768, "value out of range");
	errorCond(address < 0, "address must be >= 0");
	_memoryDumper->SetMemoryValueWord(memType, address, value, disableSideEffects);
	return l.ReturnCount();
}

int LuaApi::ConvertAddress(lua_State *lua)
{
	LuaCallHelper l(lua);
	l.ForceParamCount(3);
	CpuType cpuType = (CpuType)l.ReadInteger((uint32_t)_context->GetDefaultCpuType());
	MemoryType memType = (MemoryType)l.ReadInteger((uint32_t)_context->GetDefaultMemType());
	uint32_t address = l.ReadInteger();
	checkminparams(1);

	checkEnum(CpuType, cpuType, "invalid cpu type");
	checkEnum(MemoryType, memType, "invalid memory type");
	errorCond(address < 0 || address >= _memoryDumper->GetMemorySize(memType), "address is out of range");

	AddressInfo src { (int32_t)address, memType };
	AddressInfo result;
	if(DebugUtilities::IsRelativeMemory(memType)) {
		result = _debugger->GetAbsoluteAddress(src);
		//todo result when MemoryType::Register?
	} else {
		result = _debugger->GetRelativeAddress(src, cpuType);
	}

	if(result.Address < 0) {
		lua_pushnil(lua);
	} else {
		lua_newtable(lua);
		lua_pushintvalue(address, result.Address);
		lua_pushintvalue(memType, result.Type);
	}
	return 1;
}

int LuaApi::GetLabelAddress(lua_State* lua)
{
	LuaCallHelper l(lua);
	string label = l.ReadString();
	checkparams();
	errorCond(label.length() == 0, "label cannot be empty");

	LabelManager* labelManager = _debugger->GetLabelManager();
	AddressInfo addr = labelManager->GetLabelAbsoluteAddress(label);
	if(addr.Address < 0) {
		//TODO multibyte label review?
		//Check to see if the label is a multi-byte label instead
		string mbLabel = label + "+0";
		addr = labelManager->GetLabelAbsoluteAddress(mbLabel);
	}

	if(addr.Address < 0) {
		lua_pushnil(lua);
	} else {
		lua_newtable(lua);
		lua_pushintvalue(address, addr.Address);
		lua_pushintvalue(memType, addr.Type);
	}
	return 1;
}

int LuaApi::RegisterMemoryCallback(lua_State *lua)
{
	LuaCallHelper l(lua);
	l.ForceParamCount(6);

	MemoryType memType = (MemoryType)l.ReadInteger((int)_context->GetDefaultMemType());
	CpuType cpuType = (CpuType)l.ReadInteger((int)_context->GetDefaultCpuType());
	int32_t endAddr = l.ReadInteger(-1);
	uint32_t startAddr = l.ReadInteger();
	CallbackType callbackType = (CallbackType)l.ReadInteger();
	int reference = l.GetReference();

	checkminparams(3);

	if(endAddr == -1) {
		endAddr = startAddr;
	}

	errorCond(startAddr < 0, "start address must be >= 0");
	errorCond(startAddr > (uint32_t)endAddr, "start address must be <= end address");
	checkEnum(CallbackType, callbackType, "invalid callback type");
	checkEnum(MemoryType, memType, "invalid memory type");
	checkEnum(CpuType, cpuType, "invalid cpu type");
	errorCond(reference == LUA_NOREF, "callback function could not be found");

	_context->RegisterMemoryCallback(callbackType, startAddr, endAddr, memType, cpuType, reference);
	_context->Log("Registered memory callback from $" + HexUtilities::ToHex((uint32_t)startAddr) + " to $" + HexUtilities::ToHex((uint32_t)endAddr));
	l.Return(reference);
	return l.ReturnCount();
}

int LuaApi::UnregisterMemoryCallback(lua_State *lua)
{
	LuaCallHelper l(lua);
	l.ForceParamCount(6);
	
	MemoryType memType = (MemoryType)l.ReadInteger((int)_context->GetDefaultMemType());
	CpuType cpuType = (CpuType)l.ReadInteger((int)_context->GetDefaultCpuType());
	int endAddr = l.ReadInteger(-1);
	int startAddr = l.ReadInteger();
	CallbackType callbackType = (CallbackType)l.ReadInteger();
	int reference = l.ReadInteger();

	checkminparams(3);

	if(endAddr == -1) {
		endAddr = startAddr;
	}

	errorCond(startAddr < 0, "start address must be >= 0");
	errorCond(startAddr > endAddr, "start address must be <= end address");
	checkEnum(CallbackType, callbackType, "invalid callback type");
	checkEnum(MemoryType, memType, "invalid memory type");
	checkEnum(CpuType, cpuType, "invalid cpu type");
	errorCond(reference == LUA_NOREF, "callback function could not be found");

	_context->UnregisterMemoryCallback(callbackType, startAddr, endAddr, memType, cpuType, reference);
	return l.ReturnCount();
}

int LuaApi::RegisterEventCallback(lua_State *lua)
{
	LuaCallHelper l(lua);
	EventType type = (EventType)l.ReadInteger();
	int reference = l.GetReference();
	checkparams();
	checkEnum(EventType, type, "invalid event type");
	errorCond(reference == LUA_NOREF, "callback function could not be found");
	_context->RegisterEventCallback(type, reference);
	l.Return(reference);
	return l.ReturnCount();
}

int LuaApi::UnregisterEventCallback(lua_State *lua)
{
	LuaCallHelper l(lua);
	EventType type = (EventType)l.ReadInteger();
	int reference = l.ReadInteger();
	checkparams();

	checkEnum(EventType, type, "invalid event type");
	errorCond(reference == LUA_NOREF, "callback function could not be found");
	_context->UnregisterEventCallback(type, reference);
	return l.ReturnCount();
}

int LuaApi::MeasureString(lua_State* lua)
{
	LuaCallHelper l(lua);
	l.ForceParamCount(2);
	int maxWidth = l.ReadInteger(0);
	string text = l.ReadString();
	checkminparams(1);

	TextSize size = DrawStringCommand::MeasureString(text, maxWidth);
	lua_newtable(lua);
	lua_pushintvalue(width, size.X);
	lua_pushintvalue(height, size.Y);
	return 1;
}

int LuaApi::DrawString(lua_State *lua)
{
	LuaCallHelper l(lua);
	l.ForceParamCount(8);
	int displayDelay = l.ReadInteger(0);
	int frameCount = l.ReadInteger(1);
	int maxWidth = l.ReadInteger(0);
	int backColor = l.ReadInteger(0);
	int color = l.ReadInteger(0xFFFFFF);
	string text = l.ReadString();
	int y = l.ReadInteger();
	int x = l.ReadInteger();
	checkminparams(3);

	int startFrame = _emu->GetFrameCount() + displayDelay;
	_emu->GetDebugHud()->DrawString(x, y, text, color, backColor, frameCount, startFrame, maxWidth);

	return l.ReturnCount();
}

int LuaApi::DrawLine(lua_State *lua)
{
	LuaCallHelper l(lua);
	l.ForceParamCount(7);
	int displayDelay = l.ReadInteger(0);
	int frameCount = l.ReadInteger(1);
	int color = l.ReadInteger(0xFFFFFF);
	int y2 = l.ReadInteger();
	int x2 = l.ReadInteger();
	int y = l.ReadInteger();
	int x = l.ReadInteger();
	checkminparams(4);

	int startFrame = _emu->GetFrameCount() + displayDelay;
	_emu->GetDebugHud()->DrawLine(x, y, x2, y2, color, frameCount, startFrame);

	return l.ReturnCount();
}

int LuaApi::DrawPixel(lua_State *lua)
{
	LuaCallHelper l(lua);
	l.ForceParamCount(5);
	int displayDelay = l.ReadInteger(0);
	int frameCount = l.ReadInteger(1);
	int color = l.ReadInteger();
	int y = l.ReadInteger();
	int x = l.ReadInteger();
	checkminparams(3);

	int startFrame = _emu->GetFrameCount() + displayDelay;
	_emu->GetDebugHud()->DrawPixel(x, y, color, frameCount, startFrame);

	return l.ReturnCount();
}

int LuaApi::DrawRectangle(lua_State *lua)
{
	LuaCallHelper l(lua);
	l.ForceParamCount(8);
	int displayDelay = l.ReadInteger(0);
	int frameCount = l.ReadInteger(1);
	bool fill = l.ReadBool(false);
	int color = l.ReadInteger(0xFFFFFF);
	int height = l.ReadInteger();
	int width = l.ReadInteger();
	int y = l.ReadInteger();
	int x = l.ReadInteger();
	checkminparams(4);

	int startFrame = _emu->GetFrameCount() + displayDelay;
	_emu->GetDebugHud()->DrawRectangle(x, y, width, height, color, fill, frameCount, startFrame);

	return l.ReturnCount();
}

int LuaApi::ClearScreen(lua_State *lua)
{
	LuaCallHelper l(lua);
	checkparams();

	_emu->GetDebugHud()->ClearScreen();
	return l.ReturnCount();
}

FrameInfo LuaApi::InternalGetScreenSize()
{
	PpuFrameInfo frame = _emu->GetPpuFrame();
	FrameInfo frameSize;
	frameSize.Height = frame.Height;
	frameSize.Width = frame.Width;

	unique_ptr<BaseVideoFilter> filter(_emu->GetVideoFilter());
	filter->SetBaseFrameInfo(frameSize);
	filter->SetOverscan({});
	return filter->GetFrameInfo();
}

int LuaApi::GetScreenSize(lua_State* lua)
{
	LuaCallHelper l(lua);

	FrameInfo size = InternalGetScreenSize();
	lua_newtable(lua);
	lua_pushintvalue(width, size.Width);
	lua_pushintvalue(height, size.Height);
	return 1;
}

int LuaApi::GetScreenBuffer(lua_State *lua)
{
	LuaCallHelper l(lua);

	PpuFrameInfo frame = _emu->GetPpuFrame();
	FrameInfo frameSize;
	frameSize.Height = frame.Height;
	frameSize.Width = frame.Width;
	
	unique_ptr<BaseVideoFilter> filter(_emu->GetVideoFilter());
	filter->SetBaseFrameInfo(frameSize);
	frameSize = filter->SendFrame((uint16_t*)frame.FrameBuffer, _emu->GetFrameCount(), nullptr, false);
	uint32_t* rgbBuffer = filter->GetOutputBuffer();

	lua_createtable(lua, frameSize.Height*frameSize.Width, 0);
	for(int32_t i = 0, len = frameSize.Height * frameSize.Width; i < len; i++) {
		lua_pushinteger(lua, rgbBuffer[i] & 0xFFFFFF);
		lua_rawseti(lua, -2, i + 1);
	}

	return 1;
}

int LuaApi::SetScreenBuffer(lua_State *lua)
{
	LuaCallHelper l(lua);
	
	FrameInfo size = InternalGetScreenSize();

	int startFrame = _emu->GetFrameCount();
	unique_ptr<DrawScreenBufferCommand> cmd(new DrawScreenBufferCommand(size.Width, size.Height, startFrame));

	luaL_checktype(lua, 1, LUA_TTABLE);
	for(int i = 0, len = size.Height * size.Width; i < len; i++) {
		lua_rawgeti(lua, 1, i+1);
		uint32_t color = (uint32_t)lua_tointeger(lua, -1);
		lua_pop(lua, 1);
		cmd->SetPixel(i, color ^ 0xFF000000);
	}
	
	_emu->GetDebugHud()->AddCommand(std::move(cmd));
	return l.ReturnCount();
}

int LuaApi::GetPixel(lua_State *lua)
{
	LuaCallHelper l(lua);
	int y = l.ReadInteger();
	int x = l.ReadInteger();
	checkparams();
	//TODO
	errorCond(x < 0 || x > 255 || y < 0 || y > 238, "invalid x,y coordinates (must be between 0-255, 0-238)");

	//int multiplier = _ppu->IsHighResOutput() ? 2 : 1;

	//TODO
	//l.Return(DefaultVideoFilter::ToArgb(*(_ppu->GetScreenBuffer() + y * 256 * multiplier * multiplier + x * multiplier)) & 0xFFFFFF);
	return l.ReturnCount();
}

int LuaApi::GetMouseState(lua_State *lua)
{
	LuaCallHelper l(lua);
	MousePosition pos = KeyManager::GetMousePosition();
	checkparams();
	lua_newtable(lua);
	lua_pushintvalue(x, pos.X);
	lua_pushintvalue(y, pos.Y);
	lua_pushboolvalue(left, KeyManager::IsMouseButtonPressed(MouseButton::LeftButton));
	lua_pushboolvalue(middle, KeyManager::IsMouseButtonPressed(MouseButton::MiddleButton));
	lua_pushboolvalue(right, KeyManager::IsMouseButtonPressed(MouseButton::RightButton));
	return 1;
}

int LuaApi::Log(lua_State *lua)
{
	LuaCallHelper l(lua);
	string text = l.ReadString();
	checkparams();
	_context->Log(text);
	return l.ReturnCount();
}

int LuaApi::DisplayMessage(lua_State *lua)
{
	LuaCallHelper l(lua);
	string text = l.ReadString();
	string category = l.ReadString();
	checkparams();
	MessageManager::DisplayMessage(category, text);
	return l.ReturnCount();
}

int LuaApi::Reset(lua_State *lua)
{
	LuaCallHelper l(lua);
	checkparams();
	checkinitdone();
	//TODO should this use the action manager reset instead?
	_emu->Reset();
	return l.ReturnCount();
}

int LuaApi::Break(lua_State *lua)
{
	LuaCallHelper l(lua);
	checkparams();
	checkinitdone();
	_debugger->Step(_context->GetDefaultCpuType(), 1, StepType::Step);
	return l.ReturnCount();
}

int LuaApi::Resume(lua_State *lua)
{
	LuaCallHelper l(lua);
	checkparams();
	checkinitdone();
	_debugger->Run();
	return l.ReturnCount();
}

int LuaApi::Execute(lua_State *lua)
{
	LuaCallHelper l(lua);
	StepType type = (StepType)l.ReadInteger();
	int count = l.ReadInteger();
	checkparams();
	checkinitdone();
	errorCond(count <= 0, "count must be >= 1");
	errorCond(type != StepType::Step && type != StepType::PpuStep, "type is invalid");

	_debugger->Step(_context->GetDefaultCpuType(), count, type);

	return l.ReturnCount();
}

int LuaApi::Rewind(lua_State *lua)
{
	LuaCallHelper l(lua);
	int seconds = l.ReadInteger();
	checkparams();
	checksavestateconditions();
	errorCond(seconds <= 0, "seconds must be >= 1");
	_emu->GetRewindManager()->RewindSeconds(seconds);
	return l.ReturnCount();
}

int LuaApi::TakeScreenshot(lua_State *lua)
{
	LuaCallHelper l(lua);
	checkparams();
	stringstream ss;
	_emu->GetVideoDecoder()->TakeScreenshot(ss);
	l.Return(ss.str());
	return l.ReturnCount();
}

int LuaApi::IsKeyPressed(lua_State *lua)
{
	LuaCallHelper l(lua);
	string keyName = l.ReadString();
	checkparams();
	uint32_t keyCode = KeyManager::GetKeyCode(keyName);
	errorCond(keyCode == 0, "Invalid key name");
	l.Return(KeyManager::IsKeyPressed(keyCode));
	return l.ReturnCount();
}

int LuaApi::GetInput(lua_State *lua)
{
	LuaCallHelper l(lua);
	l.ForceParamCount(2);
	int subport = l.ReadInteger(0);
	int port = l.ReadInteger();
	checkminparams(1);

	errorCond(port < 0 || port > 5, "Invalid port number - must be between 0 to 4");
	errorCond(subport < 0 || subport > IControllerHub::MaxSubPorts, "Invalid subport number");

	shared_ptr<BaseControlDevice> controller = _emu->GetControlManager()->GetControlDevice(port, subport);

	lua_newtable(lua);

	if(controller) {
		vector<DeviceButtonName> buttons = controller->GetKeyNameAssociations();
		for(DeviceButtonName& btn : buttons) {
			lua_pushstring(lua, btn.Name.c_str());
			if(btn.IsNumeric) {
				if(btn.ButtonId == BaseControlDevice::DeviceXCoordButtonId) {
					lua_pushinteger(lua, controller->GetCoordinates().X);
				} else if(btn.ButtonId == BaseControlDevice::DeviceYCoordButtonId) {
					lua_pushinteger(lua, controller->GetCoordinates().Y);
				}
			} else {
				lua_pushboolean(lua, controller->IsPressed(btn.ButtonId));
			}
			lua_settable(lua, -3);
		}
	}

	return 1;
}

int LuaApi::SetInput(lua_State* lua)
{
	LuaCallHelper l(lua);
	l.ForceParamCount(4);
	lua_settop(lua, 4);

	bool allowUserInput = l.ReadBool(false);
	int subport = l.ReadInteger(0);
	int port = l.ReadInteger();

	errorCond(port < 0 || port > 5, "Invalid port number - must be between 0 to 4");
	errorCond(subport < 0 || subport > IControllerHub::MaxSubPorts, "Invalid subport number");

	shared_ptr<BaseControlDevice> controller = _emu->GetControlManager()->GetControlDevice(port, subport);
	if(!controller) {
		return 0;
	}

	luaL_checktype(lua, 1, LUA_TTABLE);

	vector<DeviceButtonName> buttons = controller->GetKeyNameAssociations();
	for(DeviceButtonName& btn : buttons) {
		lua_getfield(lua, 1, btn.Name.c_str());
		if(btn.IsNumeric) {
			Nullable<int32_t> btnState = l.ReadOptionalInteger();
			if(btnState.HasValue || !allowUserInput) {
				if(btn.ButtonId == BaseControlDevice::DeviceXCoordButtonId) {
					MousePosition pos = controller->GetCoordinates();
					pos.X = (int16_t)btnState.Value;
					controller->SetCoordinates(pos);
				} else if(btn.ButtonId == BaseControlDevice::DeviceYCoordButtonId) {
					MousePosition pos = controller->GetCoordinates();
					pos.Y = (int16_t)btnState.Value;
					controller->SetCoordinates(pos);
				}
			}
		} else {
			Nullable<bool> btnState = l.ReadOptionalBool();
			if(btnState.HasValue || !allowUserInput) {
				controller->SetBitValue(btn.ButtonId, btnState.Value);
			}
		}
	}
	
	lua_pop(lua, 1);

	return l.ReturnCount();
}


int LuaApi::GetAccessCounters(lua_State *lua)
{
	LuaCallHelper l(lua);
	l.ForceParamCount(2);
	MemoryOperationType operationType = (MemoryOperationType)l.ReadInteger();
	MemoryType memoryType = (MemoryType)l.ReadInteger();
	errorCond(operationType >= MemoryOperationType::ExecOperand, "Invalid operation type");
	errorCond(memoryType == MemoryType::Register, "Invalid memory type");
	checkEnum(MemoryType, memoryType, "Invalid memory type");

	checkparams();

	uint32_t size = 0;
	vector<AddressCounters> counts;
	counts.resize(_memoryDumper->GetMemorySize(memoryType), {});
	_debugger->GetMemoryAccessCounter()->GetAccessCounts(0, size, memoryType, counts.data());

	lua_createtable(lua, size, 0);
	switch(operationType) {
		default:
		case MemoryOperationType::Read: 
			for(uint32_t i = 0; i < size; i++) {
				lua_pushinteger(lua, counts[i].ReadCounter);
				lua_rawseti(lua, -2, i + 1);
			}
			break;

		case MemoryOperationType::Write:
			for(uint32_t i = 0; i < size; i++) {
				lua_pushinteger(lua, counts[i].WriteCounter);
				lua_rawseti(lua, -2, i + 1);
			}
			break;

		case MemoryOperationType::ExecOpCode:
			for(uint32_t i = 0; i < size; i++) {
				lua_pushinteger(lua, counts[i].ExecCounter);
				lua_rawseti(lua, -2, i + 1);
			}
			break;
	}
	return 1;
}

int LuaApi::ResetAccessCounters(lua_State *lua)
{
	LuaCallHelper l(lua);
	checkparams();
	_debugger->GetMemoryAccessCounter()->ResetCounts();
	return l.ReturnCount();
}

int LuaApi::GetScriptDataFolder(lua_State *lua)
{
	LuaCallHelper l(lua);
	checkparams();
	string baseFolder = FolderUtilities::CombinePath(FolderUtilities::GetHomeFolder(), "LuaScriptData");
	FolderUtilities::CreateFolder(baseFolder);
	string scriptFolder = FolderUtilities::CombinePath(baseFolder, FolderUtilities::GetFilename(_context->GetScriptName(), false));
	FolderUtilities::CreateFolder(scriptFolder);
	l.Return(scriptFolder);
	return l.ReturnCount();
}

int LuaApi::GetRomInfo(lua_State *lua)
{
	LuaCallHelper l(lua);
	checkparams();

	RomInfo romInfo = _emu->GetRomInfo();

	lua_newtable(lua);
	lua_pushstringvalue(name, romInfo.RomFile.GetFileName());
	lua_pushstringvalue(path, romInfo.RomFile.GetFilePath());
	lua_pushstringvalue(fileSha1Hash, _emu->GetHash(HashType::Sha1));

	return 1;
}

int LuaApi::GetLogWindowLog(lua_State *lua)
{
	LuaCallHelper l(lua);
	checkparams();
	
	l.Return(MessageManager::GetLog());
	return l.ReturnCount();
}

int LuaApi::GetState(lua_State *lua)
{
	LuaCallHelper l(lua);
	checkparams();

	Serializer s(0, true, SerializeFormat::Map);
	s.Stream(*_emu->GetConsole(), "", -1);
	
	//Add some more Lua-specific values
	uint32_t clockRate = _emu->GetMasterClockRate();
	string consoleType = string(magic_enum::enum_name<ConsoleType>(_emu->GetConsoleType()));
	string region = string(magic_enum::enum_name<ConsoleRegion>(_emu->GetRegion()));
	
	SV(clockRate);
	SV(consoleType);
	SV(region);

	unordered_map<string, SerializeMapValue>& values = s.GetMapValues();

	lua_newtable(lua);
	for(auto& kvp : values) {
		lua_pushstring(lua, kvp.first.c_str());
		switch(kvp.second.Format) {
			case SerializeMapValueFormat::Integer: lua_pushinteger(lua, kvp.second.Value.Integer); break;
			case SerializeMapValueFormat::Double: lua_pushnumber(lua, kvp.second.Value.Double); break;
			case SerializeMapValueFormat::Bool: lua_pushboolean(lua, kvp.second.Value.Bool); break;
			case SerializeMapValueFormat::String: lua_pushstring(lua, kvp.second.StringValue.c_str()); break;
		}
		lua_settable(lua, -3);
	}
	return 1;
}

int LuaApi::SetState(lua_State* lua)
{
	LuaCallHelper l(lua);
	lua_settop(lua, 1);
	luaL_checktype(lua, -1, LUA_TTABLE);

	unordered_map<string, SerializeMapValue> map;

	lua_pushnil(lua);  /* first key */
	while(lua_next(lua, -2) != 0) {
		/* uses 'key' (at index -2) and 'value' (at index -1) */
		if(lua_type(lua, -2) == LUA_TSTRING) {
			size_t len = 0;
			const char* cstr = lua_tolstring(lua, -2, &len);
			string key = string(cstr, len);

			switch(lua_type(lua, -1)) {
				case LUA_TBOOLEAN: {
					map.try_emplace(key, SerializeMapValueFormat::Bool, (bool)lua_toboolean(lua, -1));
					break;
				}

				case LUA_TNUMBER: {
					if(lua_isinteger(lua, -1)) {
						map.try_emplace(key, SerializeMapValueFormat::Integer, (int64_t)lua_tointeger(lua, -1));
					} else if(lua_isnumber(lua, -1)) {
						map.try_emplace(key, SerializeMapValueFormat::Double, (double)lua_tonumber(lua, -1));
					}
					break;
				}
			}
		}
		
		/* removes 'value'; keeps 'key' for next iteration */
		lua_pop(lua, 1);
	}

	Serializer s(0, false, SerializeFormat::Map);
	s.LoadFromMap(map);

	s.Stream(*_emu->GetConsole(), "", -1);
	unordered_map<string, SerializeMapValue>& values = s.GetMapValues();

	lua_newtable(lua);
	for(auto& kvp : values) {
		lua_pushstring(lua, kvp.first.c_str());
		switch(kvp.second.Format) {
			case SerializeMapValueFormat::Integer: lua_pushinteger(lua, kvp.second.Value.Integer); break;
			case SerializeMapValueFormat::Double: lua_pushnumber(lua, kvp.second.Value.Double); break;
			case SerializeMapValueFormat::Bool: lua_pushboolean(lua, kvp.second.Value.Bool); break;
		}
		lua_settable(lua, -3);
	}
	return 1;
}