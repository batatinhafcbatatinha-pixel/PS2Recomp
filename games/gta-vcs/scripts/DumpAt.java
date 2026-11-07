// Headless: disassemble instruction ranges. args: <outfile> <start>:<end> ...
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.*;
import ghidra.program.model.listing.*;
import java.io.*;
public class DumpAt extends GhidraScript {
  public void run() throws Exception {
    String[] a = getScriptArgs();
    PrintWriter out = new PrintWriter(new FileWriter(a[0]));
    Listing listing = currentProgram.getListing();
    for (int i = 1; i < a.length; i++) {
      String[] p = a[i].split(":");
      Address start = toAddr(p[0]);
      Address end = toAddr(p[1]);
      out.println("==== " + a[i]);
      InstructionIterator it = listing.getInstructions(start, true);
      while (it.hasNext()) {
        Instruction ins = it.next();
        if (ins.getAddress().compareTo(end) > 0) break;
        out.println(ins.getAddress() + ": " + ins.toString());
      }
    }
    out.close();
  }
}
