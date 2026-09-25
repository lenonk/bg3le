#pragma once

namespace bg3le {

class SymbolTable;

void noesis_set_symbols(const SymbolTable* symbols);

// The one Noesis::View, and its content: the root upstream's Ext.UI.GetRoot
// returns. Null until the UI exists.
void* noesis_view();
void* noesis_root();

// FrameworkElement::FindName and GetName.
void* noesis_find_name(void* element, char const* name);
char const* noesis_name(void* element);

}  // namespace bg3le
