// Exports what Ghidra's decompiler recovers at every bctr of the current program, as TSV: an independent cross-check of
// the jump tables proved from the code by tools/decomp_functions.py (jump_analysis). Compared by the project's test; the
// output holds addresses taken from the game: it goes under generated/, never into Git.
//
//   site    <bctr address>  <function entry, or NONE>  <function end>  <block name:offset of the bctr>
//   table   <bctr address>  <function entry>  <switch address>  <label=destination,...>  <table address:entry size:count;...>
//   nothing <bctr address>  <function entry>  <reason>
//
// HighFunction.getJumpTables() needs the jump loads for the load tables (DecompInterface.toggleJumpLoads, Ghidra 12.1
// sources of JumpTable and DecompInterface). A destination without a label is written with the label "-".
//
// Headless usage: -postScript ExportJumpTables.java <output.tsv> [<entries.txt>]
// entries.txt: one hexadecimal function entry per line. An entry that is in no function of the program (code that
// auto-analysis did not reach, a function only called through a pointer) gets one for this run: run the script read-only
// (analyze.py script --read-only) and nothing is saved.
//@category GameCube

import java.io.File;
import java.io.PrintWriter;
import java.nio.file.Files;
import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;

import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileOptions;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.listing.InstructionIterator;
import ghidra.program.model.mem.MemoryBlock;
import ghidra.program.model.pcode.HighFunction;
import ghidra.program.model.pcode.JumpTable;

public class ExportJumpTables extends GhidraScript {

	private static final int TIMEOUT_SECONDS = 120;

	private String place(Address a) {
		MemoryBlock block = currentProgram.getMemory().getBlock(a);
		return block == null ? "?" : block.getName() + ":" + Long.toHexString(a.subtract(block.getStart()));
	}

	@Override
	public void run() throws Exception {
		String[] args = getScriptArgs();
		if (args.length < 1) {
			printerr("ExportJumpTables: the output path is required as an argument");
			return;
		}
		File out = new File(args[0]);
		out.getParentFile().mkdirs();
		if (args.length > 1) {
			for (String line : Files.readAllLines(new File(args[1]).toPath())) {
				Address entry = toAddr(Long.parseLong(line.trim(), 16));
				if (getFunctionContaining(entry) == null) {
					disassemble(entry);
					createFunction(entry, null);
				}
			}
		}

		Map<Function, List<Address>> sites = new LinkedHashMap<>();
		List<Address> orphans = new ArrayList<>();
		InstructionIterator it = currentProgram.getListing().getInstructions(true);
		while (it.hasNext() && !monitor.isCancelled()) {
			Instruction i = it.next();
			if (!"bctr".equals(i.getMnemonicString())) {
				continue;
			}
			Function f = getFunctionContaining(i.getAddress());
			if (f == null) {
				orphans.add(i.getAddress());
			}
			else {
				sites.computeIfAbsent(f, k -> new ArrayList<>()).add(i.getAddress());
			}
		}

		DecompileOptions options = new DecompileOptions();
		options.setMaxInstructions(Integer.MAX_VALUE);
		DecompInterface decompiler = new DecompInterface();
		decompiler.setOptions(options);
		decompiler.toggleCCode(true);
		decompiler.toggleSyntaxTree(true);
		decompiler.toggleJumpLoads(true);
		if (!decompiler.openProgram(currentProgram)) {
			printerr("ExportJumpTables: the decompiler could not open the program");
			return;
		}
		int tables = 0;
		try (PrintWriter w = new PrintWriter(out, "UTF-8")) {
			w.println("# kind\tbctr\t...");
			for (Address a : orphans) {
				w.printf("site\t%s\tNONE\t-\t%s%n", a, place(a));
				w.printf("nothing\t%s\tNONE\tno function here%n", a);
			}
			for (Map.Entry<Function, List<Address>> e : sites.entrySet()) {
				if (monitor.isCancelled()) {
					break;
				}
				Function f = e.getKey();
				String entry = f.getEntryPoint().toString();
				for (Address a : e.getValue()) {
					w.printf("site\t%s\t%s\t%s\t%s%n", a, entry, f.getBody().getMaxAddress(), place(a));
				}
				DecompileResults r = decompiler.decompileFunction(f, TIMEOUT_SECONDS, monitor);
				HighFunction hf = r == null ? null : r.getHighFunction();
				if (hf == null) {
					String why = r == null ? "no result" : r.getErrorMessage().replace('\n', ' ').replace('\t', ' ');
					for (Address a : e.getValue()) {
						w.printf("nothing\t%s\t%s\t%s%n", a, entry, why);
					}
					continue;
				}
				List<Address> found = new ArrayList<>();
				for (JumpTable t : hf.getJumpTables()) {
					Address[] cases = t.getCases();
					Integer[] labels = t.getLabelValues();
					StringBuilder dests = new StringBuilder();
					for (int i = 0; i < cases.length; i++) {
						dests.append(i == 0 ? "" : ",");
						dests.append(i < labels.length ? labels[i].toString() : "-").append('=').append(cases[i]);
					}
					StringBuilder loads = new StringBuilder();
					for (JumpTable.LoadTable l : t.getLoadTables()) {
						loads.append(loads.length() == 0 ? "" : ";");
						loads.append(l.getAddress()).append(':').append(l.getSize()).append(':').append(l.getNum());
					}
					w.printf("table\t%s\t%s\t%s\t%s\t%s%n", t.getSwitchAddress(), entry, t.getSwitchAddress(), dests,
						loads);
					found.add(t.getSwitchAddress());
					tables++;
				}
				for (Address a : e.getValue()) {
					if (!found.contains(a)) {
						w.printf("nothing\t%s\t%s\tno jump table recovered here%n", a, entry);
					}
				}
			}
		}
		decompiler.dispose();
		println("ExportJumpTables: " + tables + " tables -> " + out);
	}
}
