// @category PS2Recomp
import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.FunctionIterator;
import java.io.PrintWriter;
public class ExportBen10FunctionsOnly extends GhidraScript {
    public void run() throws Exception {
        String[] args = getScriptArgs();
        if (args.length != 1) throw new IllegalArgumentException("Expected output CSV");
        int count = 0;
        try (PrintWriter out = new PrintWriter(args[0])) {
            out.println("Name,Start,End,Size");
            FunctionIterator it = currentProgram.getFunctionManager().getFunctions(true);
            while (it.hasNext()) {
                Function f = it.next();
                if (f.getBody().isEmpty()) continue;
                out.printf("%s,0x%08X,0x%08X,%d%n", f.getName(),
                    f.getEntryPoint().getOffset(), f.getBody().getMaxAddress().getOffset()+1,
                    f.getBody().getNumAddresses());
                count++;
            }
        }
        println("Exported " + count + " FunctionManager functions; synthetic labels excluded");
    }
}
