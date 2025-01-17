/* SPDX-License-Identifier: MIT */

#include "asm/output.hpp"

#include <algorithm>
#include <assert.h>
#include <deque>
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>

#include "error.hpp"

#include "asm/charmap.hpp"
#include "asm/fstack.hpp"
#include "asm/main.hpp"
#include "asm/rpn.hpp"
#include "asm/section.hpp"
#include "asm/symbol.hpp"
#include "asm/warning.hpp"

struct Assertion {
	Patch patch;
	Section *section;
	std::string message;
};

char const *objectName;

// List of symbols to put in the object file
static std::vector<Symbol *> objectSymbols;

static std::deque<Assertion> assertions;

static std::deque<FileStackNode *> fileStackNodes;

// Write a long to a file (little-endian)
static void putlong(uint32_t n, FILE *f) {
	uint8_t bytes[] = {
	    (uint8_t)n,
	    (uint8_t)(n >> 8),
	    (uint8_t)(n >> 16),
	    (uint8_t)(n >> 24),
	};
	fwrite(bytes, 1, sizeof(bytes), f);
}

// Write a NUL-terminated string to a file
static void putstring(std::string const &s, FILE *f) {
	fputs(s.c_str(), f);
	putc('\0', f);
}

void out_RegisterNode(FileStackNode *node) {
	// If node is not already registered, register it (and parents), and give it a unique ID
	for (; node && node->ID == (uint32_t)-1; node = node->parent) {
		node->ID = fileStackNodes.size();
		fileStackNodes.push_front(node);
	}
}

void out_ReplaceNode(FileStackNode * /* node */) {
#if 0
This is code intended to replace a node, which is pretty useless until ref counting is added...

	auto search = std::find(RANGE(fileStackNodes), node);
	assert(search != fileStackNodes.end());
	// The list is supposed to have decrementing IDs; catch inconsistencies early
	assert(search->ID == node->ID);
	assert(search + 1 == fileStackNodes.end() || (search + 1)->ID == node->ID - 1);

	// TODO: unreference the node
	*search = node;
#endif
}

// Return a section's ID, or -1 if the section is not in the list
static uint32_t getSectIDIfAny(Section *sect) {
	if (!sect)
		return (uint32_t)-1;

	if (auto search = sectionMap.find(sect->name); search != sectionMap.end())
		return (uint32_t)(sectionMap.size() - search->second - 1);

	fatalerror("Unknown section '%s'\n", sect->name.c_str());
}

// Write a patch to a file
static void writepatch(Patch const &patch, FILE *f) {
	assert(patch.src->ID != (uint32_t)-1);
	putlong(patch.src->ID, f);
	putlong(patch.lineNo, f);
	putlong(patch.offset, f);
	putlong(getSectIDIfAny(patch.pcSection), f);
	putlong(patch.pcOffset, f);
	putc(patch.type, f);
	putlong(patch.rpn.size(), f);
	fwrite(patch.rpn.data(), 1, patch.rpn.size(), f);
}

// Write a section to a file
static void writesection(Section const &sect, FILE *f) {
	putstring(sect.name, f);

	putlong(sect.size, f);

	bool isUnion = sect.modifier == SECTION_UNION;
	bool isFragment = sect.modifier == SECTION_FRAGMENT;

	putc(sect.type | isUnion << 7 | isFragment << 6, f);

	putlong(sect.org, f);
	putlong(sect.bank, f);
	putc(sect.align, f);
	putlong(sect.alignOfs, f);

	if (sect_HasData(sect.type)) {
		fwrite(sect.data.data(), 1, sect.size, f);
		putlong(sect.patches.size(), f);

		for (Patch const &patch : sect.patches)
			writepatch(patch, f);
	}
}

// Write a symbol to a file
static void writesymbol(Symbol const &sym, FILE *f) {
	putstring(sym.name, f);
	if (!sym.isDefined()) {
		putc(SYMTYPE_IMPORT, f);
	} else {
		assert(sym.src->ID != (uint32_t)-1);

		putc(sym.isExported ? SYMTYPE_EXPORT : SYMTYPE_LOCAL, f);
		putlong(sym.src->ID, f);
		putlong(sym.fileLine, f);
		putlong(getSectIDIfAny(sym.getSection()), f);
		putlong(sym.getOutputValue(), f);
	}
}

static void registerSymbol(Symbol &sym) {
	sym.ID = objectSymbols.size();
	objectSymbols.push_back(&sym);
	out_RegisterNode(sym.src);
}

// Returns a symbol's ID within the object file
// If the symbol does not have one, one is assigned by registering the symbol
static uint32_t getSymbolID(Symbol &sym) {
	if (sym.ID == (uint32_t)-1 && !sym_IsPC(&sym))
		registerSymbol(sym);
	return sym.ID;
}

static void writerpn(std::vector<uint8_t> &rpnexpr, std::vector<uint8_t> const &rpn) {
	std::string symName;
	size_t rpnptr = 0;

	for (size_t offset = 0; offset < rpn.size();) {
		uint8_t rpndata = rpn[offset++];

		switch (rpndata) {
			Symbol *sym;
			uint32_t value;
			uint8_t b;

		case RPN_CONST:
			rpnexpr[rpnptr++] = RPN_CONST;
			rpnexpr[rpnptr++] = rpn[offset++];
			rpnexpr[rpnptr++] = rpn[offset++];
			rpnexpr[rpnptr++] = rpn[offset++];
			rpnexpr[rpnptr++] = rpn[offset++];
			break;

		case RPN_SYM:
			symName.clear();
			for (;;) {
				uint8_t c = rpn[offset++];
				if (c == 0)
					break;
				symName += c;
			}

			// The symbol name is always written expanded
			sym = sym_FindExactSymbol(symName.c_str());
			if (sym->isConstant()) {
				rpnexpr[rpnptr++] = RPN_CONST;
				value = sym_GetConstantValue(symName.c_str());
			} else {
				rpnexpr[rpnptr++] = RPN_SYM;
				value = getSymbolID(*sym);
			}

			rpnexpr[rpnptr++] = value & 0xFF;
			rpnexpr[rpnptr++] = value >> 8;
			rpnexpr[rpnptr++] = value >> 16;
			rpnexpr[rpnptr++] = value >> 24;
			break;

		case RPN_BANK_SYM:
			symName.clear();
			for (;;) {
				uint8_t c = rpn[offset++];
				if (c == 0)
					break;
				symName += c;
			}

			// The symbol name is always written expanded
			sym = sym_FindExactSymbol(symName.c_str());
			value = getSymbolID(*sym);

			rpnexpr[rpnptr++] = RPN_BANK_SYM;
			rpnexpr[rpnptr++] = value & 0xFF;
			rpnexpr[rpnptr++] = value >> 8;
			rpnexpr[rpnptr++] = value >> 16;
			rpnexpr[rpnptr++] = value >> 24;
			break;

		case RPN_BANK_SECT:
			rpnexpr[rpnptr++] = RPN_BANK_SECT;
			do {
				b = rpn[offset++];
				rpnexpr[rpnptr++] = b;
			} while (b != 0);
			break;

		case RPN_SIZEOF_SECT:
			rpnexpr[rpnptr++] = RPN_SIZEOF_SECT;
			do {
				b = rpn[offset++];
				rpnexpr[rpnptr++] = b;
			} while (b != 0);
			break;

		case RPN_STARTOF_SECT:
			rpnexpr[rpnptr++] = RPN_STARTOF_SECT;
			do {
				b = rpn[offset++];
				rpnexpr[rpnptr++] = b;
			} while (b != 0);
			break;

		default:
			rpnexpr[rpnptr++] = rpndata;
			break;
		}
	}
}

static void initpatch(Patch &patch, uint32_t type, Expression const &expr, uint32_t ofs) {
	FileStackNode *node = fstk_GetFileStack();

	patch.type = type;
	patch.src = node;
	// All patches are assumed to eventually be written, so the file stack node is registered
	out_RegisterNode(node);
	patch.lineNo = lexer_GetLineNo();
	patch.offset = ofs;
	patch.pcSection = sect_GetSymbolSection();
	patch.pcOffset = sect_GetSymbolOffset();

	if (expr.isKnown) {
		// If the RPN expr's value is known, output a constant directly
		patch.rpn.resize(5);
		patch.rpn[0] = RPN_CONST;
		patch.rpn[1] = (uint32_t)expr.val & 0xFF;
		patch.rpn[2] = (uint32_t)expr.val >> 8;
		patch.rpn[3] = (uint32_t)expr.val >> 16;
		patch.rpn[4] = (uint32_t)expr.val >> 24;
	} else {
		patch.rpn.resize(expr.rpnPatchSize);
		writerpn(patch.rpn, expr.rpn);
	}
}

// Create a new patch (includes the rpn expr)
void out_CreatePatch(uint32_t type, Expression const &expr, uint32_t ofs, uint32_t pcShift) {
	// Add the patch to the list
	Patch &patch = currentSection->patches.emplace_front();

	initpatch(patch, type, expr, ofs);

	// If the patch had a quantity of bytes output before it,
	// PC is not at the patch's location, but at the location
	// before those bytes.
	patch.pcOffset -= pcShift;
}

// Creates an assert that will be written to the object file
void out_CreateAssert(
    AssertionType type, Expression const &expr, char const *message, uint32_t ofs
) {
	Assertion &assertion = assertions.emplace_front();

	initpatch(assertion.patch, type, expr, ofs);
	assertion.message = message;
}

static void writeassert(Assertion &assert, FILE *f) {
	writepatch(assert.patch, f);
	putstring(assert.message, f);
}

static void writeFileStackNode(FileStackNode const &node, FILE *f) {
	putlong(node.parent ? node.parent->ID : (uint32_t)-1, f);
	putlong(node.lineNo, f);
	putc(node.type, f);
	if (node.type != NODE_REPT) {
		putstring(node.name(), f);
	} else {
		std::vector<uint32_t> const &nodeIters = node.iters();

		putlong(nodeIters.size(), f);
		// Iters are stored by decreasing depth, so reverse the order for output
		for (uint32_t i = nodeIters.size(); i--;)
			putlong(nodeIters[i], f);
	}
}

static void registerUnregisteredSymbol(Symbol &sym) {
	// Check for symbol->src, to skip any built-in symbol from rgbasm
	if (sym.src && sym.ID == (uint32_t)-1) {
		registerSymbol(sym);
	}
}

// Write an objectfile
void out_WriteObject() {
	FILE *f;

	if (strcmp(objectName, "-")) {
		f = fopen(objectName, "wb");
	} else {
		objectName = "<stdout>";
		f = fdopen(STDOUT_FILENO, "wb");
	}
	if (!f)
		err("Failed to open object file '%s'", objectName);

	// Also write symbols that weren't written above
	sym_ForEach(registerUnregisteredSymbol);

	fprintf(f, RGBDS_OBJECT_VERSION_STRING);
	putlong(RGBDS_OBJECT_REV, f);

	putlong(objectSymbols.size(), f);
	putlong(sectionList.size(), f);

	putlong(fileStackNodes.size(), f);
	for (auto it = fileStackNodes.begin(); it != fileStackNodes.end(); it++) {
		FileStackNode const *node = *it;

		writeFileStackNode(*node, f);

		// The list is supposed to have decrementing IDs
		if (it + 1 != fileStackNodes.end() && it[1]->ID != node->ID - 1)
			fatalerror(
			    "Internal error: fstack node #%" PRIu32 " follows #%" PRIu32
			    ". Please report this to the developers!\n",
			    it[1]->ID,
			    node->ID
			);
	}

	for (Symbol const *sym : objectSymbols)
		writesymbol(*sym, f);

	for (auto it = sectionList.rbegin(); it != sectionList.rend(); it++)
		writesection(*it, f);

	putlong(assertions.size(), f);

	for (Assertion &assert : assertions)
		writeassert(assert, f);

	fclose(f);
}

// Set the objectfilename
void out_SetFileName(char *s) {
	if (objectName)
		warnx("Overriding output filename %s", objectName);
	objectName = s;
	if (verbose)
		printf("Output filename %s\n", objectName);
}
