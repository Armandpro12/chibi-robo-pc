// Lists the functions Ghidra marks as no-return, to find the wrong ones. A wrong flag cuts the pseudo-C of every caller
// right after the call. Only Ghidra's own views suffer: tools/decomp_functions.py walks the bytes itself.
// Report only: nothing is changed in the program.
//
// Columns (TSV):
//   entry            function
//   name             its name
//   returns_in_body  return instructions in its body: above 0, the flag is wrong
//   first_return     address of the first one, or "-"
//   returns_after    return instructions between the end of its body and the next function. A function that calls
//                    another wrongly flagged one loses its tail and looks as if it never returned, so its own body
//                    holds no return: this column is the only sign of such a cascade.
//   callers          number of functions that call it
// A return is any form of `bclr` without the link bit (`blr` and its conditional forms).
//
// Headless usage: -postScript ExportNoReturn.java <output.tsv>
//@category GameCube

import java.io.File;
import java.io.PrintWriter;

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.FunctionIterator;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.listing.InstructionIterator;
import ghidra.program.model.mem.Memory;
import ghidra.program.model.mem.MemoryAccessException;
import ghidra.program.model.mem.MemoryBlock;

public class ExportNoReturn extends GhidraScript {

	// bclr: primary opcode 19, extended opcode 16, link bit clear; the condition fields are free.
	private static final int BCLR_MASK = 0xFC0007FF;
	private static final int BCLR_VALUE = 0x4C000020;
	private static final int INSTRUCTION_SIZE = 4;

	@Override
	public void run() throws Exception {
		String[] args = getScriptArgs();
		if (args.length < 1) {
			printerr("ExportNoReturn: the output path is required as an argument");
			return;
		}
		File out = new File(args[0]);
		out.getParentFile().mkdirs();
		int flagged = 0, withReturn = 0, withTail = 0;
		try (PrintWriter w = new PrintWriter(out, "UTF-8")) {
			w.println("# entry\tname\treturns_in_body\tfirst_return\treturns_after\tcallers");
			FunctionIterator it = currentProgram.getFunctionManager().getFunctions(true);
			while (it.hasNext() && !monitor.isCancelled()) {
				Function f = it.next();
				if (!f.hasNoReturn()) {
					continue;
				}
				flagged++;
				long first = -1;
				int inBody = 0;
				InstructionIterator instructions = currentProgram.getListing().getInstructions(f.getBody(), true);
				while (instructions.hasNext()) {
					Instruction i = instructions.next();
					if (isReturn(i.getInt(0))) {
						inBody++;
						first = first < 0 ? i.getAddress().getOffset() : first;
					}
				}
				int after = returnsAfterBody(f);
				withReturn += inBody > 0 ? 1 : 0;
				withTail += after > 0 ? 1 : 0;
				w.printf("%08X\t%s\t%d\t%s\t%d\t%d%n", f.getEntryPoint().getOffset(), f.getName(), inBody,
					first < 0 ? "-" : String.format("%08X", first), after, f.getCallingFunctions(monitor).size());
			}
		}
		println(String.format("ExportNoReturn: %d no-return functions, %d with a return in their body, " +
			"%d with a return after it -> %s", flagged, withReturn, withTail, out));
	}

	private static boolean isReturn(int word) {
		return (word & BCLR_MASK) == BCLR_VALUE;
	}

	/** Returns in the bytes between the end of the body and the next function, or the end of the block. */
	private int returnsAfterBody(Function f) throws MemoryAccessException {
		Memory memory = currentProgram.getMemory();
		Address end = f.getBody().getMaxAddress();
		MemoryBlock block = memory.getBlock(end);
		if (block == null || !block.isInitialized()) {
			return 0;
		}
		long stop = block.getEnd().getOffset() + 1;
		Function next = getFunctionAfter(f);
		if (next != null) {
			stop = Math.min(stop, next.getEntryPoint().getOffset());
		}
		int count = 0;
		// Whole words only: the code of this platform is aligned on 4 bytes.
		for (long a = (end.getOffset() | (INSTRUCTION_SIZE - 1)) + 1; a + INSTRUCTION_SIZE <= stop; a += INSTRUCTION_SIZE) {
			if (isReturn(memory.getInt(toAddr(a), true))) {
				count++;
			}
		}
		return count;
	}
}
