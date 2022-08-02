#include "stdafx.h"
#include "Debugger/ScriptManager.h"
#include "Debugger/ScriptHost.h"
#include "Debugger/DebugBreakHelper.h"
#include "Debugger/Debugger.h"
#include "Shared/Emulator.h"
#include "Shared/Video/DebugHud.h"
#include "MemoryOperationType.h"

ScriptManager::ScriptManager(Debugger* debugger)
{
	_debugger = debugger;
	_hasScript = false;
	_nextScriptId = 1;
}

int ScriptManager::LoadScript(string name, string content, int32_t scriptId)
{
	DebugBreakHelper helper(_debugger);
	auto lock = _scriptLock.AcquireSafe();

	if(scriptId < 0) {
		unique_ptr<ScriptHost> script(new ScriptHost(_nextScriptId++));
		script->LoadScript(name, content, _debugger);
		scriptId = script->GetScriptId();
		_scripts.push_back(std::move(script));
		_hasScript = true;
		return scriptId;
	} else {
		auto result = std::find_if(_scripts.begin(), _scripts.end(), [=](unique_ptr<ScriptHost> &script) {
			return script->GetScriptId() == scriptId;
		});
		if(result != _scripts.end()) {
			//Send a ScriptEnded event before reloading the code
			(*result)->ProcessEvent(EventType::ScriptEnded);

			(*result)->LoadScript(name, content, _debugger);
			RefreshMemoryCallbackFlags();
			return scriptId;
		}
	}

	return -1;
}

void ScriptManager::RemoveScript(int32_t scriptId)
{
	DebugBreakHelper helper(_debugger);
	auto lock = _scriptLock.AcquireSafe();
	_scripts.erase(std::remove_if(_scripts.begin(), _scripts.end(), [=](const unique_ptr<ScriptHost>& script) {
		if(script->GetScriptId() == scriptId) {
			//Send a ScriptEnded event before unloading the script
			script->ProcessEvent(EventType::ScriptEnded);
			_debugger->GetEmulator()->GetDebugHud()->ClearScreen();
			return true;
		}
		return false;
	}), _scripts.end());

	RefreshMemoryCallbackFlags();

	_hasScript = _scripts.size() > 0;
}

void ScriptManager::RefreshMemoryCallbackFlags()
{
	_isPpuMemoryCallbackEnabled = false;
	_isCpuMemoryCallbackEnabled = false;
	for(unique_ptr<ScriptHost>& script : _scripts) {
		script->RefreshMemoryCallbackFlags();
	}
}

string ScriptManager::GetScriptLog(int32_t scriptId)
{
	auto lock = _scriptLock.AcquireSafe();
	for(unique_ptr<ScriptHost> &script : _scripts) {
		if(script->GetScriptId() == scriptId) {
			return script->GetLog();
		}
	}
	return "";
}

void ScriptManager::ProcessEvent(EventType type)
{
	for(unique_ptr<ScriptHost> &script : _scripts) {
		script->ProcessEvent(type);
	}
}

