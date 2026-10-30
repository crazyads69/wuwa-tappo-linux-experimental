import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.*;
import ghidra.program.model.symbol.*;
import ghidra.program.model.listing.*;
import java.io.*;
import java.nio.file.*;
import java.util.*;
public class Xr extends GhidraScript {
  public void run() throws Exception {
    String[] a = getScriptArgs();
    List<String> needles = Files.readAllLines(Paths.get(a[0]));
    try (PrintWriter w = new PrintWriter(a[1])) {
      for (String n : needles) {
        if (n.isEmpty()) continue;
        Address s = currentProgram.getMinAddress(); int hits=0;
        while (hits<6) {
          s = currentProgram.getMemory().findBytes(s, n.getBytes(), null, true, monitor);
          if (s==null) break; hits++;
          w.println("STR "+n+" @ "+s);
          for (Reference r : getReferencesTo(s)) { Function f=getFunctionContaining(r.getFromAddress()); w.println("  REF "+r.getFromAddress()+" fn "+(f==null?"?":f.getEntryPoint()+" size="+f.getBody().getNumAddresses())); }
          s = s.next();
        }
        if (hits==0) w.println("NOTFOUND "+n);
      }
    }
  }
}
