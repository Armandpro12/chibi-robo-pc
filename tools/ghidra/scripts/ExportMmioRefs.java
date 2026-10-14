// Exports every reference from the code to the hardware registers (0xCC000000-0xCC00FFFF)
// and to the OS low globals (0x80000000-0x800030FF) as TSV:
//   function  function_name  instruction  target  block  ref_type  instruction_text
// The references come from Ghidra's analysis (propagation of the lis/addi/ori constants)
// and, in addition, from a minimal local propagation "lis rX,0xCC00 ; op d(rX)".
// Generic: useful to recognize the SDK functions by the registers they touch.
//
// Headless usage: -postScript ExportMmioRefs.java <output.tsv>
//@category GameCube

import java.io.File;
import java.io.PrintWriter;
import java.util.HashMap;
import java.util.HashSet;
import java.util.Map;
import java.util.Set;

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.listing.InstructionIterator;
import ghidra.program.model.mem.MemoryBlock;
import ghidra.program.model.symbol.Reference;

public class ExportMmioRefs extends GhidraScript {

	private static boolean interesting(long a) {
		return (a >= 0xCC000000L && a < 0xCC010000L) || (a >= 0x80000000L && a < 0x80003100L) ||
			(a >= 0xC0000000L && a < 0xC0003100L);
	}

	@Override
	public void run() throws Exception {
		String[] args = getScriptArgs();
		if (args.length < 1) {
			printerr("ExportMmioRefs: the output path is required as an argument");
			return;
		}
		File out = new File(args[0]);
		out.getParentFile().mkdirs();
		int count = 0;
		try (PrintWriter w = new PrintWriter(out, "UTF-8")) {
			w.println("# function\tname\tinstruction\ttarget\tblock\ttype\ttext");
			InstructionIterator it = currentProgram.getListing().getInstructions(true);
			// Local propagation: register -> high value loaded by lis (reset at each function).
			Map<Integer, Long> hi = new HashMap<>();
			Function current = null;
			while (it.hasNext() && !monitor.isCancelled()) {
				Instruction ins = it.next();
				Function f = getFunctionContaining(ins.getAddress());
				if (f != current) {
					hi.clear();
					current = f;
				}
				Set<Long> seen = new HashSet<>();
				for (Reference r : ins.getReferencesFrom()) {
					Address to = r.getToAddress();
					if (!to.isMemoryAddress()) {
						continue;
					}
					long a = to.getOffset();
					if (!interesting(a) || !seen.add(a)) {
						continue;
					}
					count += emit(w, f, ins, a, r.getReferenceType().toString());
				}
				// Complement: raw decoding (lis / addi / ori then a d(rX) access), linear walk.
				int word = getInt(ins.getAddress());
				int op = (word >>> 26) & 0x3F;
				int rd = (word >>> 21) & 31;
				int ra = (word >>> 16) & 31;
				int imm = (short) (word & 0xFFFF);
				Long base = (ra != 0) ? hi.get(ra) : null;
				boolean mem = (op >= 32 && op <= 55);
				if (mem && base != null) {
					long a = (base + imm) & 0xFFFFFFFFL;
					if (interesting(a) && seen.add(a)) {
						boolean store = (op >= 36 && op <= 39) || op == 44 || op == 45 || op == 47 ||
							(op >= 52 && op <= 55);
						count += emit(w, f, ins, a, store ? "WRITE(raw)" : "READ(raw)");
					}
				}
				// Value produced by the instruction (if computable), otherwise invalidation.
				Long produced = null;
				int dest = writtenGpr(op, rd, ra);
				if (op == 15 && ra == 0) {
					produced = ((long) (word & 0xFFFF)) << 16;                 // lis
				}
				else if (op == 14 && base != null) {
					produced = (base + imm) & 0xFFFFFFFFL;                      // addi rD, rA(known), d
					if (interesting(produced) && seen.add(produced)) {
						count += emit(w, f, ins, produced, "ADDR(raw)");
					}
				}
				else if (op == 24 && hi.get(rd) != null) {
					produced = hi.get(rd) | (word & 0xFFFF);                    // ori rA, rS(known), u
				}
				if (dest == -2) {
					hi.remove(rd);
					hi.remove(ra);
				}
				else if (dest >= 0) {
					hi.remove(dest);
				}
				if (produced != null && dest >= 0) {
					hi.put(dest, produced);
				}
				// A called function may clobber the volatile registers r0, r3-r12.
				if (op == 18 && (word & 1) != 0) {
					hi.keySet().removeIf(r -> r == 0 || (r >= 3 && r <= 12));
				}
			}
		}
		println("ExportMmioRefs: " + count + " references -> " + out);
	}

	/** General-purpose register written by the instruction (-1 if none or unknown). */
	private static int writtenGpr(int op, int rd, int ra) {
		switch (op) {
			case 10: case 11: case 16: case 18: case 19:     // comparisons, branches
			case 36: case 38: case 44: case 47:              // stw, stb, sth, stmw
			case 48: case 50: case 52: case 54:              // lfs, lfd, stfs, stfd
			case 56: case 60:                                // psq_l, psq_st
				return -1;
			case 24: case 25: case 26: case 27: case 28: case 29: case 20: case 21: case 23:
				return ra;                                   // ori/oris/xori/xoris/andi./andis./rlwimi/rlwinm/rlwnm
			case 37: case 39: case 45: case 49: case 51: case 53: case 55: case 57: case 61:
				return ra;                                   // "with update" forms
			case 31:
				return -2;                                   // X form: handled separately (caution)
			default:
				return rd;
		}
	}

	private int emit(PrintWriter w, Function f, Instruction ins, long a, String kind) {
		MemoryBlock b = currentProgram.getMemory().getBlock(toAddr(a));
		w.printf("%s\t%s\t%08X\t%08X\t%s\t%s\t%s%n",
			f == null ? "-" : String.format("%08X", f.getEntryPoint().getOffset()),
			f == null ? "-" : f.getName(),
			ins.getAddress().getOffset(), a, b == null ? "-" : b.getName(), kind, ins.toString());
		return 1;
	}
}
