/*
 * Copyright (c) 2012, 2013 Erik Faye-Lund
 * Copyright (c) 2013 Avionic Design GmbH
 * Copyright (c) 2013 Thierry Reding
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#ifdef HAVE_CONFIG_H
#  include "config.h"
#endif

#include <ctype.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#include "libcgc.h"
#include "host1x.h"

#include "libcgc-private.h"

#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))

static const char swizzle[4] = { 'x', 'y', 'z', 'w' };

/*
 *           00  01
 * uniforms:
 *   bool:  45a cb8
 *   int:   445 cb8
 *   uint:  445 cb8
 *   float: 415 cb8
 *
 *   bvec2: 45c cb8
 *   bvec3: 45d cb8
 *   bvec4: 45e cb8
 *
 *   ivec2: 447 cb8
 *   ivec3: 448
 *   ivec4: 449
 *
 *   vec2: 416
 *   vec3: 417
 *   vec4: 418
 *
 *   mat2: 41e
 *   mat3: 423
 *   mat4: 428
 *
 *   sampler2D: 42a
 *   sampler3D: 42b
 *
 * attribute:
 *   vec4: 418 841
 *
 * other:
 *   gl_Position: 418 8c3
 *   gl_PointSize: 415 905
 *   0.12345: 443 882
 */

static const struct data_type {
	enum glsl_type glsl;
	unsigned int type;
	const char *name;
} data_types[] = {
	{ GLSL_TYPE_FLOAT, 0x01, "mediump float" },
	{ GLSL_TYPE_VEC2, 0x02, "mediump vec2" },
	{ GLSL_TYPE_VEC3, 0x03, "mediump vec3" },
	{ GLSL_TYPE_VEC4, 0x04, "mediump vec4" },
	{ GLSL_TYPE_MAT2, 0x0a, "mediump mat2" },
	{ GLSL_TYPE_MAT3, 0x0f, "mediump mat3" },
	{ GLSL_TYPE_MAT4, 0x14, "mediump mat4" },
	{ GLSL_TYPE_FLOAT, 0x15, "highp float" },
	{ GLSL_TYPE_VEC2, 0x16, "highp vec2" },
	{ GLSL_TYPE_VEC3, 0x17, "highp vec3" },
	{ GLSL_TYPE_VEC4, 0x18, "highp vec4" },
	{ GLSL_TYPE_MAT2, 0x1e, "highp mat2" },
	{ GLSL_TYPE_MAT3, 0x23, "highp mat3" },
	{ GLSL_TYPE_MAT4, 0x28, "highp mat4" },
	{ GLSL_TYPE_SAMPLER2D, 0x2a, "sampler2D" },
	{ GLSL_TYPE_SAMPLER3D, 0x2b, "sampler3D" },
	{ GLSL_TYPE_SAMPLER3D, 0x2d, "samplerCube" },
	{ GLSL_TYPE_FLOAT, 0x2e, "lowp float" },
	{ GLSL_TYPE_VEC2, 0x2f, "lowp vec2" },
	{ GLSL_TYPE_VEC3, 0x30, "lowp vec3" },
	{ GLSL_TYPE_VEC4, 0x31, "lowp vec4" },
	{ GLSL_TYPE_MAT2, 0x37, "lowp mat2" },
	{ GLSL_TYPE_MAT3, 0x3c, "lowp mat3" },
	{ GLSL_TYPE_MAT4, 0x41, "lowp mat4" },
	{ GLSL_TYPE_INT, 0x45, "int" },
	{ GLSL_TYPE_IVEC2, 0x47, "ivec2" },
	{ GLSL_TYPE_IVEC3, 0x48, "ivec3" },
	{ GLSL_TYPE_IVEC4, 0x49, "ivec4" },
	{ GLSL_TYPE_BOOL, 0x5a, "bool" },
	{ GLSL_TYPE_BVEC2, 0x5c, "bvec2" },
	{ GLSL_TYPE_BVEC3, 0x5d, "bvec3" },
	{ GLSL_TYPE_BVEC4, 0x5e, "bvec4" },
	{ GLSL_TYPE_SAMPLER3D, 0x73, "sampler2DArray" },
};

static const char *data_type_name(unsigned int type)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(data_types); i++)
		if (data_types[i].type == type)
			return data_types[i].name;

	return "unknown";
}

static const char *variable_type_name(unsigned int type)
{
	switch (type) {
	case 0x1005:
		return "attribute";

	case 0x1006:
		return "uniform";

	case 0x1007:
		return "constant";
	}

	return "unknown";
}

static int shader_parse_symbols(struct cgc_shader *shader)
{
	struct cgc_header *header = shader->binary;
	struct cgc_symbol *symbol;
	unsigned int i, j;

	shader->symbols = calloc(header->num_symbols, sizeof(*symbol));
	if (!shader->symbols)
		return -ENOMEM;

	shader->num_symbols = header->num_symbols;

	for (i = 0; i < header->num_symbols; i++) {
		struct cgc_header_symbol *sym = &header->symbols[i];
		struct cgc_symbol *symbol = &shader->symbols[i];
		enum glsl_type glsl = GLSL_TYPE_UNKNOWN;
		const char *name = NULL;

		if (sym->name_offset)
			name = shader->binary + sym->name_offset;

		for (j = 0; j < ARRAY_SIZE(data_types); j++) {
			uint8_t type = sym->unknown00 & 0xff;

			if (type == data_types[i].type) {
				glsl = data_types[i].glsl;
				break;
			}
		}

		switch (sym->unknown02) {
		case 0x1005:
			/*
			 * unknown01:
			 *   0x84x: vertex attribute
			 *   0x8cx: builtin outputs
			 *   0xc9x: varying
			 */

			if (sym->unknown01 & (1 << 10)) {
				/* varying */
				symbol->location = sym->unknown03;
				symbol->input = false;
			} else if (sym->unknown01 & (1 << 7)) {
				/* builtin output */
				symbol->location = sym->unknown03;
				symbol->input = false;
			} else {
				/* vertex attribute */
				symbol->location = (sym->unknown01 & 0x1f) - 1;
				symbol->input = true;
			}

			symbol->kind = GLSL_KIND_ATTRIBUTE;
			break;

		case 0x1006:
			symbol->location = sym->unknown03;
			symbol->kind = GLSL_KIND_UNIFORM;
			break;

		case 0x1007:
			if (sym->values_offset) {
				const uint32_t *values = shader->binary +
							 sym->values_offset;
				for (j = 0; j < 4; j++)
					symbol->vector[j] = values[j];
			} else {
				fprintf(stderr, "no values for constant %s\n",
					name);
			}

			symbol->location = sym->unknown03;
			symbol->kind = GLSL_KIND_CONSTANT;
			break;

		default:
			symbol->kind = GLSL_KIND_UNKNOWN;
			break;
		}

		symbol->name = name ? strdup(name) : NULL;
		symbol->type = glsl;

		if (sym->unknown10 == 0x00000001)
			symbol->used = true;
		else
			symbol->used = false;
	}

	return 0;
}

static void *memdup(const void *ptr, size_t size)
{
	void *dup;

	dup = malloc(size);
	if (!dup)
		return NULL;

	memcpy(dup, ptr, size);

	return dup;
}

static void vertex_shader_disassemble(struct cgc_shader *shader, FILE *fp)
{
	struct cgc_header *header = shader->binary;
	const uint32_t *ptr;
	unsigned int i, j;

	ptr = shader->binary + header->binary_offset;
	printf("  instructions:\n");

	for (i = 0; i < header->binary_size; i += 16) {
		uint8_t sx, sy, sz, sw, neg, op, constant, attribute, varying, abs;
		uint8_t wx, wy, wz, ww, sat, write_varying, write_pred;
		uint8_t pred, pneg, psx, psy, psz, psw;
		struct instruction *inst;
		uint32_t words[4], reg, type;

		for (j = 0; j < 4; j++)
			words[j] = *ptr++;

		printf("    ");

		for (j = 0; j < 4; j++)
			printf("%08x", words[j]);

		printf(" |");

		for (j = 0; j < 4; j++)
			printf(" %08x", words[j]);

		printf("\n");

		inst = instruction_create_from_words(words, 4);

		constant = instruction_extract(inst, 76, 83);
		attribute = instruction_extract(inst, 72, 75);
		varying = instruction_extract(inst, 2, 5);
		printf("      constant #%02x\n", constant);
		printf("      attribute #%02x\n", attribute);
		printf("      varying #%02x\n", varying);

		pred = instruction_get_bit(inst, 109);
		pneg = instruction_get_bit(inst, 107);
		psx = instruction_extract(inst, 104, 105);
		psy = instruction_extract(inst, 102, 103);
		psz = instruction_extract(inst, 100, 101);
		psw = instruction_extract(inst, 98, 99);

		write_varying = instruction_get_bit(inst, 126);
		write_pred = instruction_get_bit(inst, 125);
		sat = instruction_get_bit(inst, 122);

		wx = instruction_get_bit(inst, 16);
		wy = instruction_get_bit(inst, 15);
		wz = instruction_get_bit(inst, 14);
		ww = instruction_get_bit(inst, 13);

		if (wx || wy || wz || ww) {
			int rb = 1, rc = 0; /* most opcodes use 2 operands */
			printf("      vec op\n");
			printf("        ");

			if (pred) {
				printf("(%sp0.%c%c%c%c) ", pneg ? "!" : "",
				       swizzle[psx], swizzle[psy], swizzle[psz], swizzle[psw]);
			}

			op = instruction_extract(inst, 86, 90);
			switch (op) {
			case 0x1:
				printf("mov");
				rb = 0;
				break;
			case 0x2:
				printf("mul");
				break;
			case 0x3:
				printf("add");
				rb = 0;
				rc = 1;
				break;
			case 0x4:
				printf("mad");
				rc = 1;
				break;
			case 0x5:
				printf("dp3");
				break;
			case 0x7:
				printf("dp4");
				break;
			case 0x9:
				printf("min");
				break;
			case 0xa:
				printf("max");
				break;
			case 0xb:
				printf("slt");
				break;
			case 0xc:
				printf("sge");
				break;
			case 0xd:
				printf("arl");
				rb = 0;
				break;
			case 0xe:
				printf("frc");
				rb = 0;
				break;
			case 0xf:
				printf("flr");
				rb = 0;
				break;
			case 0x10:
				printf("seq");
				break;
			case 0x12:
				printf("sgt");
				break;
			case 0x13:
				printf("sle");
				break;
			case 0x14:
				printf("sne");
				break;
			default:
				printf("unknown(%x)", op);
				rc = 1; /* let's be verbose and output all possible operands */
				break;
			}

			reg = instruction_extract(inst, 111, 116);

			if (write_varying && reg == 0x3f)
				printf(" o%d", varying);
			else if (write_pred && reg == 0x3f)
				printf(" p0");
			else
				printf(" r%d", reg);
			if (sat)
				printf("_sat");
			printf(".%s%s%s%s", wx ? "x" : "",
			       wy ? "y" : "", wz ? "z" : "", ww ? "w" : "");

			neg = instruction_get_bit(inst, 71);
			sx = instruction_extract(inst, 69, 70);
			sy = instruction_extract(inst, 67, 68);
			sz = instruction_extract(inst, 65, 66);
			sw = instruction_extract(inst, 63, 64);
			abs = instruction_get_bit(inst, 117);
			reg = instruction_extract(inst, 57, 62);

			type = instruction_extract(inst, 55, 56);
			if (type == 2)
				reg = attribute;
			else if (type == 3)
				reg = constant;

			printf(", %s%s%c%d.%c%c%c%c%s",
			       neg ? "-" : "", abs ? "abs(" : "", "?rvc"[type], reg,
			       swizzle[sx], swizzle[sy], swizzle[sz], swizzle[sw],
			       abs ? ")" : "");

			if (rb) {
				neg = instruction_get_bit(inst, 54);
				sx = instruction_extract(inst, 52, 53);
				sy = instruction_extract(inst, 50, 51);
				sz = instruction_extract(inst, 48, 49);
				sw = instruction_extract(inst, 46, 47);
				abs = instruction_get_bit(inst, 118);
				reg = instruction_extract(inst, 40, 45);

				type = instruction_extract(inst, 38, 39);
				if (type == 2)
					reg = attribute;
				else if (type == 3)
					reg = constant;

				printf(", %s%s%c%d.%c%c%c%c%s",
				       neg ? "-" : "", abs ? "abs(" : "", "?rvc"[type], reg,
				       swizzle[sx], swizzle[sy], swizzle[sz], swizzle[sw],
				       abs ? ")" : "");
			}

			if (rc) {
				neg = instruction_get_bit(inst, 37);
				sx = instruction_extract(inst, 35, 36);
				sy = instruction_extract(inst, 33, 34);
				sz = instruction_extract(inst, 31, 32);
				sw = instruction_extract(inst, 29, 30);
				abs = instruction_get_bit(inst, 119);
				reg = instruction_extract(inst, 23, 28);

				type = instruction_extract(inst, 21, 22);
				if (type == 2)
					reg = attribute;
				else if (type == 3)
					reg = constant;

				printf(", %s%s%c%d.%c%c%c%c%s",
				       neg ? "-" : "", abs ? "abs(" : "", "?rvc"[type], reg,
				       swizzle[sx], swizzle[sy], swizzle[sz], swizzle[sw],
				       abs ? ")" : "");
			}
			printf("\n");
		}

		wx = instruction_get_bit(inst, 20);
		wy = instruction_get_bit(inst, 19);
		wz = instruction_get_bit(inst, 18);
		ww = instruction_get_bit(inst, 17);

		if (wx || wy || wz || ww) {
			printf("      scalar op\n");
			printf("        ");

			if (pred) {
				printf("(%sp0.%c%c%c%c) ", pneg ? "!" : "",
				       swizzle[psx], swizzle[psy], swizzle[psz], swizzle[psw]);
			}

			op = instruction_extract(inst, 91, 94);
			switch (op) {
			case 0x0:
				printf("cos");
				break;
			case 0x1:
				printf("mov");
				break;
			case 0x2:
				printf("rcp");
				break;
			case 0x4:
				printf("rsq");
				break;
			case 0xd:
				printf("lg2");
				break;
			case 0xe:
				printf("ex2");
				break;
			case 0xf:
				printf("sin");
				break;
			default:
				printf("unknown(%x)", op);
				break;
			}

			reg = instruction_extract(inst, 7, 12);

			if (write_varying && reg == 0x3f)
				printf(" o%d", varying);
			else if (write_pred && reg == 0x3f)
				printf(" p0");
			else
				printf(" r%d", reg);
			if (sat)
				printf("_sat");
			printf(".%s%s%s%s", wx ? "x" : "",
			       wy ? "y" : "", wz ? "z" : "", ww ? "w" : "");

			neg = instruction_get_bit(inst, 37);
			sx = instruction_extract(inst, 35, 36);
			sy = instruction_extract(inst, 33, 34);
			sz = instruction_extract(inst, 31, 32);
			sw = instruction_extract(inst, 29, 30);
			abs = instruction_get_bit(inst, 119);
			reg = instruction_extract(inst, 23, 28);

			type = instruction_extract(inst, 21, 22);
			if (type == 2)
				reg = attribute;
			else if (type == 3)
				reg = constant;

			printf(", %s%s%c%d.%c%s\n",
			       neg ? "-" : "", abs ? "abs(" : "", "?rvc"[type], reg,
			       swizzle[sx], abs ? ")" : "");
		}

		if (instruction_get_bit(inst, 0))
			printf("    done\n");

		instruction_free(inst);
	}
}

static int gpr_written[256];

#define pr(fmt, ...) do { str += sprintf(str, fmt, ##__VA_ARGS__); } while (0)

static int fragment_alu_disasm(uint32_t *words)
{
	int i, op, reg, subreg, sat, scale, accum;
	int embedded_constant_used = 0;
	struct instruction *inst;
	char buf[512], *str = buf;
	const char *dscale_str[] = {
		"", "_mul2", "_mul4", "_div2"
	};

	inst = instruction_create_from_words(words, 2);

	if (words[0] == 0x000fe7e8 && words[1] == 0x3e41f200) {
		// a NOP is an instruction that writes 0.0 to r63
		pr("nop");
		goto out;
	}

	op = instruction_extract(inst, 62, 63);
	switch (op) {
	case 0:
		pr("mad");
		break;
	case 1:
		pr("min");
		break;
	case 2:
		pr("max");
		break;
	case 3:
		pr("cnd");
		break;
	}

	accum = instruction_get_bit(inst, 61);
	scale = instruction_extract(inst, 57, 58);
	sat = instruction_get_bit(inst, 56);
	if (instruction_get_bit(inst, 53)) {
		int cond = instruction_extract(inst, 54, 55);
		const char *cond_str[4] = {
			"_z",
			"_nz",
			"_le",
			"_lt"
		};
		pr("%s", cond_str[cond]);
	}

	reg = instruction_extract(inst, 47, 51);
	pr(" r%d%s%s%s", reg, dscale_str[scale], sat ? "_sat" : "", accum ? "+" : "");
	subreg = instruction_extract(inst, 45, 46);
	pr(".%c%c", "_h"[subreg >> 1], "_l"[subreg & 1]);

	gpr_written[reg] = 1;

	for (i = 0; i < 3; ++i) {
		int type, reg, x10, abs, neg;
		int offset = 32 - 13 * i;

		/* register type */
		type = instruction_extract(inst, offset + 11, offset + 12);

		/* modifiers */
		x10 = instruction_get_bit(inst, offset + 3);
		abs = instruction_get_bit(inst, offset + 2);
		neg = instruction_get_bit(inst, offset + 1);
		scale = instruction_get_bit(inst, offset);
		pr(", ");

		if (instruction_get_bit(inst, offset + 4)) {
			pr("unk4 ");
			assert(0);
		}

		pr("%s%s", neg ? "-" : "", abs ? "abs(" : "");
		switch (type) {
		case 0:
			/* general-purpose register */
			reg = instruction_extract(inst, offset + 5, offset + 10);
			if (reg >= 48) {
				if (reg == 48)
					pr("d0");
				else if (reg == 50)
					pr("d1");
				else if (reg == 52)
					pr("d2");
				else if (reg == 54)
					pr("d3");
				else if (reg >= 56 && reg < 62) {
					pr("ec%d", reg - 56);
					embedded_constant_used = 1;
				} else if (reg == 62)
					pr("#0");
				else if (reg == 63)
					pr("#1");
				else
					assert(0);
			} else {
				assert(x10 || !(reg & 1));
				reg = instruction_extract(inst, offset + 6, offset + 10);
				if (!gpr_written[reg])
					fprintf(stderr, "\nr%d not written!\n", reg);
				pr("r%d%s", reg, x10 ? "_half" : "");
			}
			break;

		case 1:
			/* constant register */
			reg = instruction_extract(inst, offset + 5, offset + 10);
			assert(x10 || !(reg & 1));
			pr("c%d%s", x10 ? reg : reg >> 1, x10 ? "_half" : "");
			break;

		case 2:
			/* system-variable register */
			reg = instruction_extract(inst, offset + 5, offset + 10);
			if (reg == 16 && !x10)
				pr("vPos.x");
			else if (reg == 18 && !x10)
				pr("vPos.y");
			else if (reg == 22 && x10)
				pr("vFace");
			else {
				assert(x10 || !(reg & 1));
				pr("x%d%s", reg, x10 ? "_half" : "");
			}
			break;

		default:
			/* unused encoding (?) */
			assert(0);
		}
		pr("%s%s", abs ? ")" : "", scale ? " * #2" : "");
	}

out:

	printf("     ");

	instruction_print_raw(inst);
	instruction_print_unknown(inst);

	printf("    %s\n", buf);

	instruction_free(inst);

	return embedded_constant_used;
}

static void fragment_sfu_disasm(uint32_t *words)
{
	int op, reg, var;
	struct instruction *inst;
	char buf[512], *str = buf;

	inst = instruction_create_from_words(words, 2);

	if (! words[1]) { /* use heuristic for now until we know .. */
		op = instruction_extract(inst, 54, 57);
		switch (op) {
		case 0x1:
			pr("rcp");
			break;
		case 0x2:
			pr("rsq");
			break;
		case 0x3:
			pr("log");
			break;
		case 0x4:
			pr("exp");
			break;
		case 0x5:
			pr("sqrt");
			break;
		case 0x6:
			pr("sin");
			break;
		case 0x7:
			pr("cos");
			break;
		case 0x8:
			pr("frc");
			break;
		case 0x9:
			pr("preexp");
			break;
		case 0xa:
			pr("presin");
			break;
		case 0xb:
			pr("precos");
			break;
		default:
			pr("unk%x", op);
		}

		reg = instruction_extract(inst, 58, 62);
		pr(" r%d", reg);
	} else {
		var = instruction_extract(inst, 24, 28);

		pr("var v%d", var);
	}

	printf("     ");

	instruction_print_raw(inst);
	instruction_print_unknown(inst);

	printf("    %s\n", buf);

	instruction_free(inst);
}

static const char * offset_name(uint32_t offset)
{
	switch (offset) {
	case 0x206: return "VTX";
	case 0x601: return "SFU-SCHED";
	case 0x604: return "SFU";
	case 0x701: return "TEX";
	case 0x801: return "ALU-SCHED";
	case 0x804: return "ALU";
	default:    return "???";
	}
}

static void fragment_shader_disassemble(uint32_t *words, size_t length)
{
	int i, j;
	uint32_t *sfu = NULL, *alu = NULL;
	int sfu_length = 0, alu_length = 0;
	uint32_t *sfu_sched = NULL, *alu_sched = NULL;
	int sfu_sched_length = 0, alu_sched_length = 0;

	void print_sfu(int idx, int t)
	{
		printf("SFU:");
		if (t) printf("%03d", t);
		fragment_sfu_disasm(sfu + (idx * 2));
	}

	void print_alu(int idx, int t)
	{
		int k;
		int embedded_constant_used = 0;
		for (k = 0; k < (embedded_constant_used ? 3 : 4); ++k) {
			printf("ALU:");
			if (t) printf("%03d", t);
			embedded_constant_used |= fragment_alu_disasm(alu + (idx * 8) + k * 2);
		}
	}

	for (i = 0; i < length; ) {
		uint32_t word = words[i++];
		uint32_t opcode = (word >> 28) & 0xf, offset, count;

		if (opcode != 0 && opcode != 1 && opcode != 2 && opcode != 3) {
			printf("unknown opcode %d\n", opcode);
			return;
		}

		if (opcode == 0) {
			int i, mask = word & 0x3f;
			int class_id = (word >> 6) & 0x3ff;
			offset = (word >> 16) & 0xfff;

			printf("    setclass %d %d, mask: %x\n", class_id, offset, mask);
			count = 0;
			for (i = 0; i < 6; ++i)
				if (mask & (1 << i))
					++count;
		} else if (opcode == 3) {
			int i, mask = word & 0xffff;
			offset = (word >> 16) & 0xfff;
			printf("    mask: %x\n", mask);
			count = 0;
			for (i = 0; i < 16; ++i)
				if (mask & (1 << i))
					++count;
		} else {
			offset = (word >> 16) & 0xfff;
			count = word & 0xffff;
		}

printf("----------------------------------------------------------------\n");
		printf("    upload, offset 0x%03x (%s), %d words\n", offset, offset_name(offset), count);
		switch (offset) {
		case 0x601:
			sfu_sched = words + i;
			sfu_sched_length = count;
			for (j = 0; j < count; ++j)
				printf("      0x%08x\n", words[i + j]);
			break;
		case 0x604:
			sfu = words + i;
			sfu_length = count / 2;

			printf("      sfu instructions:\n");
			for (j = 0; j < sfu_length; j++)
				print_sfu(j, 0);

			break;
		case 0x801:
			alu_sched = words + i;
			alu_sched_length = count;
			for (j = 0; j < count; ++j)
				printf("      0x%08x\n", words[i + j]);
			break;
		case 0x804:
			alu = words + i;
			alu_length = count / 8;

			printf("      alu instructions:\n");
			for (j = 0; j < alu_length; j++) {
				print_alu(j, 0);
				printf("\n");
			}

			break;
		default:
			for (j = 0; j < count; ++j)
				printf("      0x%08x\n", words[i + j]);
		}
		i += count;
	}

	if (alu_sched && sfu_sched) {
		int si = 0, ai = 0;       /* instruction index */

/* This isn't quite right.. I see sequences like (aaa-2b.fs.txt, for example):
 *    upload, offset 0x801 (ALU-SCHED), 8 words
 *      0x00000002 \     larger gap here, what would make sense is SFU first,
 *      0x0000000a |-->  followed by multiple ALU instruction groups..
 *      0x00000011 /
 *      0x00000000
 *      0x00000015
 *      0x00000000
 *      0x00000019
 *      0x00000000
 *    upload, offset 0x601 (SFU-SCHED), 8 words
 *      0x00000001
 *      0x00000000
 *      0x00000005
 *      0x00000009
 *      0x00000000
 *      0x0000000d
 *      0x00000000
 *      0x00000011
 */
		assert(alu_sched_length == sfu_sched_length);  /* I think! */

		for (i = 0; i < alu_sched_length; i++) {
			if (sfu_sched[i] && alu_sched[i]) {
				/* I guess, if same value for sfu and alu, that sfu wins? */
				print_sfu(si++, sfu_sched[i]);
				print_alu(ai++, alu_sched[i]);
			} else if (sfu_sched[i]) {
				print_sfu(si++, sfu_sched[i]);
			} else if (alu_sched[i]) {
				print_alu(ai++, alu_sched[i]);
			}
		}
	}
}

static void shader_stream_dump(struct cgc_shader *shader, FILE *fp)
{
	struct cgc_fragment_shader *fs;
	struct cgc_vertex_shader *vs;
	struct host1x_stream stream;
	struct cgc_header *header;
	size_t length;
	void *words;

	switch (shader->type) {
	case CGC_SHADER_VERTEX:
		vs = shader->stream;
		words = shader->stream + vs->unknowne8 * 4;
		length = vs->unknownec;
		break;

	case CGC_SHADER_FRAGMENT:
		header = shader->binary;
		fs = shader->binary + header->binary_offset;
		length = header->binary_size - sizeof(*fs);
		words = fs->words;

		fragment_shader_disassemble(words, length / 4);

		fprintf(fp, "signature: %.*s\n", 8, fs->signature);
		fprintf(fp, "unknown0: 0x%08x\n", fs->unknown0);
		fprintf(fp, "unknown1: 0x%08x\n", fs->unknown1);
		break;

	default:
		fprintf(fp, "unknown type: %d\n", shader->type);
		return;
	}

	fprintf(fp, "stream @%p, %zu bytes\n", words, length);
	host1x_stream_init(&stream, words, length);
	host1x_stream_dump(&stream, fp);
}

static void cgc_shader_disassemble(struct cgc_shader *shader, FILE *fp)
{
	struct cgc_header *header = shader->binary;

	switch (header->type) {
	case 0x1b5d:
		vertex_shader_disassemble(shader, fp);
		shader_stream_dump(shader, fp);
		break;

	case 0x1b5e:
		shader_stream_dump(shader, fp);
		break;
	}
}

struct cgc_shader *cgc_compile(enum cgc_shader_type type,
			       const char *code, size_t size)
{
	struct cgc_shader *shader;
	const char *shader_type;
	char source[65536];
	struct CgDrv *cg;
	size_t i;
	int err;

	switch (type) {
	case CGC_SHADER_VERTEX:
		shader_type = "vertex";
		break;

	case CGC_SHADER_FRAGMENT:
		shader_type = "fragment";
		break;

	default:
		return NULL;
	}

	cg = CgDrv_Create();
	if (!cg)
		return NULL;

	memcpy(source, code, size);

	fprintf(stdout, "compiling %s shader (%zu bytes)...\n", shader_type,
		size);
	fputs("| ", stdout);

	for (i = 0; i < size; i++) {
		fputc(code[i], stdout);

		if (code[i] == '\n' && i != size - 1)
			fputs("| ", stdout);
	}

	fputs("\n", stdout);

	err = CgDrv_Compile(cg, 1, type, source, size, 0, 0);
	if (err) {
		fprintf(stderr, "%s\n", cg->error);
		fprintf(stderr, "%s\n", cg->log);
		CgDrv_Delete(cg);
		return NULL;
	}

	printf("%s\n", cg->log);

	shader = calloc(1, sizeof(*shader));
	if (!shader) {
		CgDrv_Delete(cg);
		return NULL;
	}

	shader->type = type;

	shader->binary = memdup(cg->binary, cg->binary_size);
	shader->size = cg->binary_size;

	shader->stream = memdup(cg->stream, cg->length);
	shader->length = cg->length;

	err = shader_parse_symbols(shader);
	if (err < 0) {
		fprintf(stderr, "cannot parse symbols\n");
		CgDrv_Delete(cg);
		return NULL;
	}

	CgDrv_Delete(cg);

	return shader;
}

void cgc_shader_free(struct cgc_shader *shader)
{
	if (shader) {
		free(shader->stream);
		free(shader->binary);
	}

	free(shader);
}

struct cgc_symbol *cgc_shader_get_symbol_by_kind(struct cgc_shader *shader,
						 enum glsl_kind kind,
						 unsigned int index)
{
	unsigned int i, j;

	for (i = 0, j = 0; i < shader->num_symbols; i++) {
		struct cgc_symbol *symbol = &shader->symbols[i];

		if (symbol->kind == kind) {
			if (j == index)
				return symbol;

			j++;
		}
	}

	return NULL;
}

struct cgc_symbol *cgc_shader_find_symbol_by_kind(struct cgc_shader *shader,
						  enum glsl_kind kind,
						  const char *name,
						  unsigned int *index)
{
	unsigned int i, j;

	for (i = 0, j = 0; i < shader->num_symbols; i++) {
		struct cgc_symbol *symbol = &shader->symbols[i];

		if (symbol->kind == kind) {
			if (strcmp(name, symbol->name) == 0) {
				if (index)
					*index = j;

				return symbol;
			}

			j++;
		}
	}

	return NULL;
}

static float flt(uint32_t dword)
{
	union {
		float f;
		uint32_t d;
	} u;
	u.d = dword;
	return u.f;
}

void cgc_shader_dump(struct cgc_shader *shader, FILE *fp)
{
	struct cgc_header *header = shader->binary;
	struct cgc_symbol *symbol;
	unsigned int i, j;
	const char *type;

	cgc_shader_disassemble(shader, fp);

	fprintf(fp, "shader binary: %zu bytes\n", shader->size);

	for (i = 0; i < shader->size; i += 4) {
		uint32_t value = *(uint32_t *)(shader->binary + i);
		uint8_t *bytes = (uint8_t *)(shader->binary + i);

		fprintf(fp, "  %08x: %08x |", i, value);

		for (j = 0; j < 4; j++)
			fprintf(fp, " %02x", bytes[j]);

		fprintf(fp, " | ");

		for (j = 0; j < 4; j++) {
			if (isprint(bytes[j]))
				fprintf(fp, "%c", bytes[j]);
			else
				fprintf(fp, ".");
		}

		fprintf(fp, " |\n");
	}

	fprintf(fp, "shader stream: %zu bytes\n", shader->length);

	for (i = 0; i < shader->length; i += 4) {
		uint32_t value = *(uint32_t *)(shader->stream + i);
		uint8_t *bytes = (uint8_t *)(shader->stream + i);

		fprintf(fp, "  %08x: %08x |", i, value);

		for (j = 0; j < 4; j++)
			fprintf(fp, " %02x", bytes[j]);

		fprintf(fp, " | ");

		for (j = 0; j < 4; j++) {
			if (isprint(bytes[j]))
				fprintf(fp, "%c", bytes[j]);
			else
				fprintf(fp, ".");
		}

		fprintf(fp, " |\n");
	}

	if (header->type == 0x1b5d)
		type = "vertex";
	else if (header->type == 0x1b5e)
		type = "fragment";
	else
		type = "unknown";

	fprintf(fp, "%s shader:\n", type);
	fprintf(fp, "  type: 0x%08x\n", header->type);
	fprintf(fp, "  unknown00: 0x%08x\n", header->unknown00);
	fprintf(fp, "  size: 0x%08x\n", header->size);
	fprintf(fp, "  num_symbols: %u\n", header->num_symbols);
	fprintf(fp, "  bar_size: %u\n", header->bar_size);
	fprintf(fp, "  bar_offset: 0x%08x\n", header->bar_offset);
	fprintf(fp, "  binary_size: %u\n", header->binary_size);
	fprintf(fp, "  binary_offset: 0x%08x\n", header->binary_offset);
	fprintf(fp, "  unknown01: 0x%08x\n", header->unknown01);
	fprintf(fp, "  unknown02: 0x%08x\n", header->unknown02);
	fprintf(fp, "  unknown03: 0x%08x\n", header->unknown03);
	fprintf(fp, "  unknown04: 0x%08x\n", header->unknown04);
	fprintf(fp, "  symbols:\n");

	for (i = 0; i < header->num_symbols; i++) {
		struct cgc_header_symbol *symbol = &header->symbols[i];
		const char *name, *data_type;

		data_type = data_type_name(symbol->unknown00 & 0xff);
		name = shader->binary + symbol->name_offset;

		fprintf(fp, "    %u: %s %s\n", i, data_type, name);
		fprintf(fp, "      unknown00: 0x%08x\n", symbol->unknown00);
		fprintf(fp, "      unknown01: 0x%08x\n", symbol->unknown01);
		fprintf(fp, "      unknown02: 0x%08x (%s)\n", symbol->unknown02,
			variable_type_name(symbol->unknown02));
		fprintf(fp, "      unknown03: 0x%08x\n", symbol->unknown03);
		fprintf(fp, "      name_offset: 0x%08x\n", symbol->name_offset);
		fprintf(fp, "      values_offset: 0x%08x\n", symbol->values_offset);

		if (symbol->values_offset) {
			const uint32_t *values = shader->binary + symbol->values_offset;

			for (j = 0; j < 4; j++)
				fprintf(fp, "        0x%08x (%f)\n", values[j], flt(values[j]));
		}

		fprintf(fp, "      unknown06: 0x%08x\n", symbol->unknown06);
		fprintf(fp, "      alt_offset: 0x%08x\n", symbol->alt_offset);
		fprintf(fp, "      unknown08: 0x%08x\n", symbol->unknown08);
		fprintf(fp, "      unknown09: 0x%08x\n", symbol->unknown09);
		fprintf(fp, "      unknown10: 0x%08x\n", symbol->unknown10);
		fprintf(fp, "      unknown11: 0x%08x\n", symbol->unknown11);
	}

	fprintf(fp, "  attributes:\n");
	i = 0;

	while ((symbol = cgc_shader_get_attribute(shader, i)) != NULL) {
		fprintf(fp, "    %u: %s, location: %d\n", i, symbol->name,
			symbol->location);
		i++;
	}

	fprintf(fp, "  uniforms:\n");
	i = 0;

	while ((symbol = cgc_shader_get_uniform(shader, i)) != NULL) {
		if (header->type == 0x1b5e) {
			int bank = (symbol->location >> 16) & 0x7f;
			int location = (symbol->location >> 3) & 0x7;
			int mask = (symbol->location >> 8) & 0xf;
			fprintf(fp, "    %u: %s.%s%s%s%s @ %d,%d, location: 0x%08x\n", i,
			    symbol->name,
			    mask & 1 ? "x" : "",
			    mask & 2 ? "y" : "",
			    mask & 4 ? "z" : "",
			    mask & 8 ? "w" : "",
			    bank, location,
			    symbol->location);
		} else {
			fprintf(fp, "    %u: %s, location: 0x%08x\n", i,
			    symbol->name,
			    symbol->location);
		}
		i++;
	}

	fprintf(fp, "  constants:\n");
	i = 0;

	while ((symbol = cgc_shader_get_constant(shader, i)) != NULL) {
		fprintf(fp, "    %u: %s, location: %d\n", i, symbol->name,
			symbol->location);
		fprintf(fp, "      values:\n");

		for (j = 0; j < 4; j++)
			fprintf(fp, "        0x%08x\n", symbol->vector[j]);

		i++;
	}
}
