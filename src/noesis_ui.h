#pragma once

namespace bg3le {

class SymbolTable;

void noesis_set_symbols(const SymbolTable* symbols);

// The one Noesis::View, and its content: the root upstream's Ext.UI.GetRoot
// returns. Null until the UI exists. Upstream's Ext.UI.GetRoot is patched to
// call noesis_root.
void* noesis_view();
void* noesis_root();

}  // namespace bg3le
