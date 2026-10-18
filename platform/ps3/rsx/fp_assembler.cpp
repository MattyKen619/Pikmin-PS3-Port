// PS3 port: run-time fragment program assembly (see fp_assembler.h), packaging the output of cgcomp's
// parser and compiler (fpasm/) the way cgcomp's main.cpp writes .fpo files.

#include "fp_assembler.h"

#include "fpasm/types.h"
#include "fpasm/fpparser.h"
#include "fpasm/compilerfp.h"

namespace gfx {

bool assembleFragmentProgram(const char* text, FragmentProgram& out, const char** error)
{
	*error = NULL;
	// (the parser tokenizes in place)
	Vec<char> copy(text, text + strlen(text) + 1);
	CFPParser* parser     = create<CFPParser>();
	CCompilerFP* compiler = create<CCompilerFP>();
	bool ok               = false;
	parser->Parse(&copy[0]);
	if (parser->GetError()) {
		*error = parser->GetError();
	} else if (parser->GetInstructionCount() <= 0) {
		*error = "empty program";
	} else {
		compiler->Compile(parser);
		if (compiler->GetError()) {
			*error = compiler->GetError();
		} else {
			const int n                    = compiler->GetInstructionCount();
			const fragment_program_exec* f = compiler->GetInstructions();
			out.ucode.resize(n * 4);
			for (int i = 0; i < n; i++) {
				for (int k = 0; k < 4; k++) {
					u32 v                = f[i].data[k];
					out.ucode[i * 4 + k] = (v >> 16) | (v << 16);
				}
			}
			out.numRegs    = compiler->GetNumRegs();
			out.control    = compiler->GetFPControl();
			out.texcoords  = (u16)compiler->GetTexcoords();
			out.texcoord2D = (u16)compiler->GetTexcoord2D();
			out.texcoord3D = (u16)compiler->GetTexcoord3D();
			out.relocs.clear();
			List<fragment_program_data> relocs = compiler->GetConstRelocations();
			for (List<fragment_program_data>::iterator it = relocs.begin(); it != relocs.end(); ++it) {
				FragmentProgram::Reloc r;
				r.index = (u32)it->index;
				r.slot  = it->offset;
				out.relocs.push_back(r);
			}
			ok = true;
		}
	}
	destroy(compiler); // (before the parser: it shares the parser's parameter values)
	destroy(parser);
	return ok;
}

} // namespace gfx
