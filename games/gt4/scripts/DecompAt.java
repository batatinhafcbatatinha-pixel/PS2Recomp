// Headless: decompile the functions containing each address argument, and list their callers.
// args: <outfile> <addr>...   (read-only use: -process SLUS_213.88 -noanalysis -readOnly)
import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.*;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.*;
import ghidra.program.model.address.*;
import java.io.*;
public class DecompAt extends GhidraScript {
  public void run() throws Exception {
    String[] a = getScriptArgs();
    PrintWriter out = new PrintWriter(new FileWriter(a[0]));
    DecompInterface d = new DecompInterface(); d.openProgram(currentProgram);
    for (int i = 1; i < a.length; i++) {
      Address ad = toAddr(a[i]);
      Function f = getFunctionContaining(ad);
      out.println("==== " + a[i] + " in " + (f == null ? "none" : f.getName() + "@" + f.getEntryPoint()));
      if (f == null) continue;
      for (Reference r : getReferencesTo(f.getEntryPoint())) {
        Function c = getFunctionContaining(r.getFromAddress());
        out.println("  caller " + r.getFromAddress() + " " + (c == null ? "?" : c.getName()));
      }
      DecompileResults res = d.decompileFunction(f, 120, monitor);
      if (res.decompileCompleted()) out.println(res.getDecompiledFunction().getC());
    }
    out.close();
  }
}
