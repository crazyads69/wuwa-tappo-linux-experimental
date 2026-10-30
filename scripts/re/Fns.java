import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.*;
import ghidra.program.model.listing.*;
import java.io.*;
public class Fns extends GhidraScript {
  public void run() throws Exception {
    String[] a = getScriptArgs();
    try (PrintWriter w = new PrintWriter(a[0])) {
      AddressSet r = new AddressSet(toAddr(a[1]), toAddr(a[2]));
      for (Function f : currentProgram.getFunctionManager().getFunctions(r, true)) w.println("FN "+f.getEntryPoint()+" size="+f.getBody().getNumAddresses());
      w.println("instr@14001510f: "+getInstructionContaining(toAddr("14001510f"))+" data: "+getDataContaining(toAddr("14001510f")));
      w.println("instr@14001456c: "+getInstructionContaining(toAddr("14001456c")));
    }
  }
}
