#include "asmjit/core/codeholder.h"
#include "asmjit/core/jitruntime.h"

#include "dsp56kEmu/assembler.h"
#include "dsp56kEmu/dsp.h"
#include "dsp56kEmu/jitasmjithelpers.h"
#include "dsp56kEmu/jitblock.h"
#include "dsp56kEmu/jitblockruntimedata.h"
#include "dsp56kEmu/jitconfig.h"
#include "dsp56kEmu/jitdspmode.h"
#include "dsp56kEmu/jitemitter.h"
#include "dsp56kEmu/jitops.h"
#include "dsp56kEmu/jitoptimizer.h"
#include "dsp56kEmu/jitregtypes.h"
#include "dsp56kEmu/jitruntimedata.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
	using Clock = std::chrono::steady_clock;

	struct Program
	{
		const char* name;
		std::vector<const char*> ops;
	};

	struct CompiledBlock
	{
		dsp56k::TJitFunc func = nullptr;
		size_t codeSize = 0;
		size_t optimizerChanges = 0;
		double compileMs = 0.0;
	};

	uint64_t checksum(const dsp56k::DspRegs& regs)
	{
		uint64_t h = 1469598103934665603ull;
		auto mix = [&](uint64_t v)
		{
			h ^= v;
			h *= 1099511628211ull;
		};

		mix(static_cast<uint64_t>(regs.x.var));
		mix(static_cast<uint64_t>(regs.y.var));
		mix(static_cast<uint64_t>(regs.a.var));
		mix(static_cast<uint64_t>(regs.b.var));
		mix(regs.sr.var);
		for(size_t i = 0; i < regs.r.size(); ++i)
		{
			mix(regs.r[i].var);
			mix(regs.n[i].var);
			mix(regs.m[i].var);
		}
		return h;
	}

	void seedDsp(dsp56k::DSP& dsp)
	{
		dsp.resetHW();

		auto& regs = dsp.regs();
		regs.a.var = 0x00123456000000ll;
		regs.b.var = 0x00076543000000ll;
		regs.x.var = (0x200000ll << 24) | 0x400000; // x1=0.25, x0=0.5
		regs.y.var = (0x100000ll << 24) | 0x600000; // y1=0.125, y0=0.75
		regs.sr.var = 0;
		regs.r[0].var = 0x20;
		regs.r[1].var = 0x30;
		regs.n[0].var = 1;
		regs.n[1].var = 1;
	}

	void emitAsm(dsp56k::Assembler& assembler, dsp56k::DSP& dsp, dsp56k::JitBlock& block, dsp56k::JitOps& ops, const char* text)
	{
		const auto assembled = assembler.assemble(text);
		if(!assembled.success())
			throw std::runtime_error(std::string("Assembly failed for: ") + text);

		dsp56k::JitDspMode mode;
		mode.initialize(dsp);
		block.setMode(&mode);
		block.asm_().nop();
		ops.emit(0, assembled.word[0], assembled.wordCount > 1 ? assembled.word[1] : 0);
		block.asm_().nop();
		block.setMode(nullptr);
	}

	CompiledBlock compileBlock(
		asmjit::JitRuntime& runtime,
		dsp56k::DSP& dsp,
		dsp56k::Assembler& assembler,
		const Program& program,
		const bool optimize)
	{
		const auto start = Clock::now();

		asmjit::CodeHolder code;
		code.init(runtime.environment());

		dsp56k::AsmJitErrorHandler errorHandler;
		code.setErrorHandler(&errorHandler);

		dsp56k::JitEmitter emitter(&code);
		dsp56k::JitRuntimeData rtData;

		{
			dsp56k::JitConfig config;
			config.dynamicPeripheralAddressing = true;
			config.aguSupportBitreverse = true;
			config.enableOptimizer = optimize;

			dsp56k::JitBlock block(emitter, dsp, rtData, std::move(config));
			dsp56k::JitBlockRuntimeData blockRt;
			dsp56k::JitOps ops(block, blockRt);

			emitter.nop();
			errorHandler.setBlock(&blockRt);

			dsp56k::PushAllUsed pusher(block);
			emitter.mov(dsp56k::regDspPtr, asmjit::Imm(&dsp.regs()));

			for(const auto* op : program.ops)
				emitAsm(assembler, dsp, block, ops, op);

			ops.updateDirtyCCR();
			pusher.end();
		}

		emitter.ret();

		size_t optimizerChanges = 0;
		if(optimize)
		{
			dsp56k::JitOptimizer optimizer(emitter);
			optimizerChanges = optimizer.optimize();
		}

		emitter.finalize();

		CompiledBlock result;
		const auto err = runtime.add(&result.func, &code);
		if(err)
			throw std::runtime_error(std::string("JIT add failed for ") + program.name + ": " + asmjit::DebugUtils::errorAsString(err));

		result.codeSize = code.codeSize();
		result.optimizerChanges = optimizerChanges;
		result.compileMs = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
		return result;
	}

	double runBlock(dsp56k::DSP& dsp, dsp56k::TJitFunc func, const size_t iterations)
	{
		const auto start = Clock::now();
		for(size_t i = 0; i < iterations; ++i)
			func(&dsp.regs(), 0);
		return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
	}

	size_t parseIterations(const int argc, char* argv[])
	{
		if(argc < 2)
			return 1000000;

		const auto value = std::strtoull(argv[1], nullptr, 10);
		if(value == 0)
			throw std::runtime_error("Iteration count must be greater than zero");
		return static_cast<size_t>(value);
	}
}

int main(int argc, char* argv[])
{
	try
	{
		const auto iterations = parseIterations(argc, argv);
		const auto warmupIterations = std::min<size_t>(iterations / 20, 50000);

		static dsp56k::DefaultMemoryValidator validator;
		dsp56k::Memory mem(validator, 0x080000, 0x800000, 0x200000);
		dsp56k::Peripherals56362 peripheralsX;
		dsp56k::Peripherals56367 peripheralsY;
		dsp56k::DSP dsp(mem, &peripheralsX, &peripheralsY);
		dsp56k::Assembler assembler;
		asmjit::JitRuntime runtime;

		const std::vector<Program> programs =
		{
			{
				"mac_chain",
				{
					"mpy x0,y0,a",
					"mac x1,y1,a",
					"mac -x0,y1,a",
					"mpy x1,y0,b",
					"mac x0,y0,b",
					"add b,a",
					"rnd a",
					"tfr a,b",
				}
			},
			{
				"alu_mix",
				{
					"add b,a",
					"asl a",
					"asr a",
					"sub b,a",
					"neg a",
					"abs a",
					"tfr a,b",
					"add b,a",
					"asr a",
					"sub b,a",
				}
			},
			{
				"bit_mix",
				{
					"and x0,a",
					"or y0,a",
					"eor x0,a",
					"not a",
					"and y0,b",
					"or x0,b",
					"lsl a",
					"lsr a",
					"rol a",
					"move #>$0f0f0f,x0",
					"and x0,b",
				}
			}
		};

		std::cout << "dsp56k JIT minimal perf" << std::endl;
		std::cout << "iterations=" << iterations << ", warmup=" << warmupIterations << std::endl;
		std::cout << std::fixed << std::setprecision(3);
		std::cout
			<< "case,ops,compile_no_opt_ms,compile_opt_ms,opt_changes,code_no_opt_bytes,code_opt_bytes,"
			<< "run_no_opt_ms,run_opt_ms,ns_per_call_no_opt,ns_per_call_opt,speedup,checksum"
			<< std::endl;

		for(const auto& program : programs)
		{
			seedDsp(dsp);
			auto noOpt = compileBlock(runtime, dsp, assembler, program, false);

			seedDsp(dsp);
			auto opt = compileBlock(runtime, dsp, assembler, program, true);

			seedDsp(dsp);
			runBlock(dsp, noOpt.func, warmupIterations);
			seedDsp(dsp);
			const auto noOptMs = runBlock(dsp, noOpt.func, iterations);
			const auto noOptChecksum = checksum(dsp.regs());

			seedDsp(dsp);
			runBlock(dsp, opt.func, warmupIterations);
			seedDsp(dsp);
			const auto optMs = runBlock(dsp, opt.func, iterations);
			const auto optChecksum = checksum(dsp.regs());

			if(noOptChecksum != optChecksum)
				throw std::runtime_error(std::string("Checksum mismatch for ") + program.name);

			const auto noOptNs = noOptMs * 1000000.0 / static_cast<double>(iterations);
			const auto optNs = optMs * 1000000.0 / static_cast<double>(iterations);
			const auto speedup = optMs > 0.0 ? noOptMs / optMs : 0.0;

			std::cout
				<< program.name << ','
				<< program.ops.size() << ','
				<< noOpt.compileMs << ','
				<< opt.compileMs << ','
				<< opt.optimizerChanges << ','
				<< noOpt.codeSize << ','
				<< opt.codeSize << ','
				<< noOptMs << ','
				<< optMs << ','
				<< noOptNs << ','
				<< optNs << ','
				<< speedup << ",0x"
				<< std::hex << optChecksum << std::dec
				<< std::endl;

			runtime.release(&noOpt.func);
			runtime.release(&opt.func);
		}
	}
	catch(const std::exception& e)
	{
		std::cerr << "dsp56k_jit_perf failed: " << e.what() << std::endl;
		return 1;
	}

	return 0;
}
