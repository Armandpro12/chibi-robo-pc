// Applies an "address size name" symbol map (format accepted by dolrecomp --map) to the
// current program: creates the function if the address is in an executable block, otherwise a label.
// Lines starting with '#' are ignored. Generic: no game-specific address here.
//
// Headless usage: -preScript ApplySymbolMap.java <map.map>
//@category GameCube

import java.io.BufferedReader;
import java.io.File;
import java.io.FileReader;

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.mem.MemoryBlock;
import ghidra.program.model.symbol.SourceType;
import ghidra.program.model.symbol.Symbol;

public class ApplySymbolMap extends GhidraScript {

	@Override
	public void run() throws Exception {
		String[] args = getScriptArgs();
		if (args.length < 1) {
			printerr("ApplySymbolMap: the map path is required as an argument");
			return;
		}
		File mapFile = new File(args[0]);
		int functions = 0, labels = 0, skipped = 0;
		try (BufferedReader reader = new BufferedReader(new FileReader(mapFile))) {
			String line;
			while ((line = reader.readLine()) != null) {
				line = line.trim();
				if (line.isEmpty() || line.startsWith("#")) {
					continue;
				}
				String[] tok = line.split("\\s+");
				if (tok.length < 3) {
					skipped++;
					continue;
				}
				long addrValue;
				try {
					addrValue = Long.parseLong(tok[0].replace("0x", ""), 16);
				}
				catch (NumberFormatException e) {
					skipped++;
					continue;
				}
				String name = tok[2];
				Address addr = toAddr(addrValue);
				MemoryBlock block = currentProgram.getMemory().getBlock(addr);
				if (block == null) {
					skipped++;
					continue;
				}
				if (block.isExecute()) {
					if (getInstructionAt(addr) == null) {
						disassemble(addr);
					}
					Function f = getFunctionAt(addr);
					if (f == null) {
						f = createFunction(addr, name);
					}
					if (f != null) {
						f.setName(name, SourceType.IMPORTED);
						functions++;
						continue;
					}
				}
				Symbol s = createLabel(addr, name, true, SourceType.IMPORTED);
				if (s != null) {
					labels++;
				}
				else {
					skipped++;
				}
			}
		}
		println(String.format("ApplySymbolMap: %d functions, %d labels, %d lines skipped (%s)",
			functions, labels, skipped, mapFile.getName()));
	}
}
