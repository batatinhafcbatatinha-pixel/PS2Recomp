// Headless: list references to addresses / pointer values, and dump words at addresses.
// args: <outfile> <addr>...   (read-only)
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.*;
import ghidra.program.model.listing.*;
import ghidra.program.model.mem.*;
import ghidra.program.model.symbol.*;
import java.io.*;
import java.util.*;

public class Xrefs extends GhidraScript {
  public void run() throws Exception {
    String[] a = getScriptArgs();
    PrintWriter out = new PrintWriter(new FileWriter(a[0]));
    AddressFactory af = currentProgram.getAddressFactory();
    Memory mem = currentProgram.getMemory();
    for (int i = 1; i < a.length; i++) {
      String s = a[i].replaceFirst("^0x", "");
      long v = Long.parseLong(s, 16);
      Address ad = af.getDefaultAddressSpace().getAddress(v);
      out.println("==== " + s);
      // word dump around it
      try {
        out.printf("  words @%s:", ad);
        for (int k = -4; k < 12; k++) {
          Address x = ad.add(k * 4);
          out.printf(" %08x", mem.getInt(x));
        }
        out.println();
      } catch (Exception e) { out.println("  (no memory)"); }
      // direct references to the address
      ReferenceIterator it = currentProgram.getReferenceManager().getReferencesTo(ad);
      while (it.hasNext()) {
        Reference r = it.next();
        Address from = r.getFromAddress();
        Function f = getFunctionContaining(from);
        out.println("  ref " + from + " [" + r.getReferenceType() + "] in " + (f == null ? "?" : f.getName() + "@" + f.getEntryPoint()));
      }
      // references to the pointer value stored as data elsewhere (scan data words equal to v)
      int found = 0;
      for (Address p = mem.getMinAddress(); p != null && found < 40; p = p.add(4)) {
        try {
          if ((mem.getInt(p) & 0xffffffffL) == v) {
            Function f = getFunctionContaining(p);
            String d = "";
            try { Data dd = getDataAt(p); if (dd != null) d = " " + dd; } catch (Exception e) {}
            out.println("  data " + p + d + " in " + (f == null ? "?" : f.getName() + "@" + f.getEntryPoint()));
            found++;
          }
        } catch (Exception e) { }
        if (monitor.isCancelled()) break;
      }
    }
    out.close();
  }
}
