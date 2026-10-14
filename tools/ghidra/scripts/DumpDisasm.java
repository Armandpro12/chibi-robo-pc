// Writes the disassembly of functions (or ranges) to a text file, with the outgoing references
// and the names of the called functions. Output meant for generated/ (contains game code:
// never copy it into the repository).
//
// Headless usage: -postScript DumpDisasm.java <output.txt> <target>...
//   target = hexadecimal address (the function containing it), function name,
//            or "start-end" range (disassembled raw if there is no function).
//@category GameCube

import java.io.File;
import java.io.PrintWriter;
import java.util.List;

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.address.AddressSet;
import ghidra.program.model.address.AddressSetView;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.listing.InstructionIterator;
import ghidra.program.model.symbol.Reference;
import ghidra.program.model.symbol.Symbol;

public class DumpDisasm extends GhidraScript {

	@Override
	public void run() throws Exception {
		String[] args = getScriptArgs();
		if (args.length < 2) {
			printerr("DumpDisasm: <output> <target>...");
			return;
		}
		File out = new File(args[0]);
		out.getParentFile().mkdirs();
		try (PrintWriter w = new PrintWriter(out, "UTF-8")) {
			for (int i = 1; i < args.length; i++) {
				dumpTarget(w, args[i]);
			}
		}
		println("DumpDisasm: " + (args.length - 1) + " targets -> " + out);
	}

	private void dumpTarget(PrintWriter w, String target) throws Exception {
		AddressSetView set;
		String title;
		if (target.contains("-")) {
			String[] p = target.split("-");
			Address a = toAddr(Long.parseLong(p[0].replace("0x", ""), 16));
			Address b = toAddr(Long.parseLong(p[1].replace("0x", ""), 16));
			set = new AddressSet(a, b);
			title = "range " + a + "-" + b;
			if (getInstructionAt(a) == null) {
				disassemble(a);
			}
		}
		else {
			Function f = null;
			try {
				Address a = toAddr(Long.parseLong(target.replace("0x", ""), 16));
				f = getFunctionContaining(a);
				if (f == null) {
					disassemble(a);
					f = createFunction(a, null);
				}
			}
			catch (NumberFormatException e) {
				List<Function> fs = getGlobalFunctions(target);
				if (!fs.isEmpty()) {
					f = fs.get(0);
				}
			}
			if (f == null) {
				w.println("=== " + target + ": not found");
				return;
			}
			set = f.getBody();
			title = String.format("%s @ %08X (body %s-%s)", f.getName(), f.getEntryPoint().getOffset(),
				set.getMinAddress(), set.getMaxAddress());
		}
		w.println("=== " + title);
		InstructionIterator it = currentProgram.getListing().getInstructions(set, true);
		while (it.hasNext()) {
			Instruction ins = it.next();
			Symbol lab = getSymbolAt(ins.getAddress());
			if (lab != null && !lab.isDynamic()) {
				w.println("  " + lab.getName() + ":");
			}
			StringBuilder sb = new StringBuilder();
			for (Reference r : ins.getReferencesFrom()) {
				Address to = r.getToAddress();
				if (!to.isMemoryAddress()) {
					continue;
				}
				Function cf = getFunctionAt(to);
				Symbol s = getSymbolAt(to);
				sb.append(" -> ").append(to);
				if (cf != null) {
					sb.append(" <").append(cf.getName()).append(">");
				}
				else if (s != null) {
					sb.append(" <").append(s.getName()).append(">");
				}
			}
			w.printf("  %08X: %08X  %-40s%s%n", ins.getAddress().getOffset(), getInt(ins.getAddress()),
				ins.toString(), sb.toString());
		}
		w.println();
	}
}
