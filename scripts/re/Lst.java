import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.*;
import ghidra.program.model.listing.*;
import java.io.*;
public class Lst extends GhidraScript {
  public void run() throws Exception {
    String[] a=getScriptArgs();
    try (PrintWriter w=new PrintWriter(a[0])) {
      for (Instruction i : currentProgram.getListing().getInstructions(new AddressSet(toAddr(a[1]),toAddr(a[2])), true)) w.println(i.getAddress()+"  "+i);
    }
  }
}
