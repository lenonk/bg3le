// Linux stand-in for upstream's Lua/Libs/ClientUI/Builtins.inl.
//
// Upstream links no Noesis library, so Builtins.inl reimplements the Noesis
// functions bg3se calls, against Windows data layouts. The Linux executable
// has every one of them as a local symbol, so here each is a jump through a
// slot that bg3le_noesis_resolve() fills from .symtab. LuaDelegateCommand is
// bg3se's own class and keeps a body; its handler is held by bg3le's Ext.UI.

// bg3le_noesis_lua.inl, later in the same translation unit.
bool bg3le_ui_command_bound(void const* command);
void bg3le_ui_command_fired(void* command, void* parameter);

BEGIN_BARE_NS(Noesis)

// Upstream's re-registration switch for its own RegisterType; the game's
// RegisterType does not read it.
unsigned gAllowTypeReregistration{ 0 };

// The one function the game does not have; upstream's body.
String Vector3::ToString() const
{
    char s[100];
    sprintf_s(s, "(%f,%f,%f)", x, y, z);
    return s;
}

bool LuaDelegateCommand::CanExecute(BaseComponent* param) const
{
    return bg3le_ui_command_bound(this);
}

void LuaDelegateCommand::Execute(BaseComponent* param) const
{
    bg3le_ui_command_fired(const_cast<LuaDelegateCommand*>(this), param);
}

// Upstream's binding entry point; bg3le binds through Ext._Internal.UiSetHandler.
void LuaDelegateCommand::BindHandler(lua_State* L, lua::Ref const& handler)
{}

NS_IMPLEMENT_REFLECTION(LuaDelegateCommand)
{
}

END_BARE_NS()
