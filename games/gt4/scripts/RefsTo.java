// Headless: list references to each address argument (with containing function). args: <outfile> <addr>...
import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.*;
import ghidra.program.model.address.*;
import java.io.*;
public class RefsTo extends GhidraScript {
  public void run() throws Exception {
    String[] a = getScriptArgs();
    PrintWriter out = new PrintWriter(new FileWriter(a[0]));
    for (int i = 1; i < a.length; i++) {
      Address ad = toAddr(a[i]);
      out.println("==== refs to " + ad);
      for (Reference r : getReferencesTo(ad)) {
        Function f = getFunctionContaining(r.getFromAddress());
        out.println("  " + r.getFromAddress() + " " + r.getReferenceType() + " " + (f == null ? "?" : f.getName()));
      }
    }
    out.close();
  }
}
