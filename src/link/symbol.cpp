/* SPDX-License-Identifier: MIT */

#include "link/symbol.hpp"

#include <inttypes.h>
#include <map>
#include <stdlib.h>

#include "error.hpp"
#include "helpers.hpp"

#include "link/main.hpp"
#include "link/object.hpp"
#include "link/section.hpp"

std::map<std::string, Symbol *> symbols;

Label &Symbol::label() {
	assert(std::holds_alternative<Label>(data));
	return std::get<Label>(data);
}

Label const &Symbol::label() const {
	assert(std::holds_alternative<Label>(data));
	return std::get<Label>(data);
}

void sym_AddSymbol(Symbol &symbol) {
	Symbol *other = sym_GetSymbol(symbol.name);
	int32_t const *symValue = std::get_if<int32_t>(&symbol.data);
	int32_t const *otherValue = other ? std::get_if<int32_t>(&other->data) : nullptr;

	// Check if the symbol already exists with a different value
	if (other && !(symValue && otherValue && *symValue == *otherValue)) {
		fprintf(stderr, "error: \"%s\" both in %s from ", symbol.name.c_str(), symbol.objFileName);
		symbol.src->dump(symbol.lineNo);
		fprintf(stderr, " and in %s from ", other->objFileName);
		other->src->dump(other->lineNo);
		fputc('\n', stderr);
		exit(1);
	}

	// If not, add it (potentially replacing the previous same-value symbol)
	symbols[symbol.name] = &symbol;
}

Symbol *sym_GetSymbol(std::string const &name) {
	auto search = symbols.find(name);
	return search != symbols.end() ? search->second : nullptr;
}
