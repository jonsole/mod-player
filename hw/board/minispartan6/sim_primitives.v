// Simulation models of the Xilinx Spartan-6 primitives top.v uses, for the
// simulation and synthesis checks here. ISE uses its own.
module BUFG (input I, output O);
    assign O = I;
endmodule

// Only the configuration top.v uses: D0 = 0 on C0, D1 = 1 on C1 gives a
// clock that is the inverse of C0.
module ODDR2 #(parameter DDR_ALIGNMENT = "NONE", parameter INIT = 1'b0,
               parameter SRTYPE = "SYNC") (
    output Q, input C0, input C1, input CE, input D0, input D1, input R, input S
);
    assign Q = C0 ? D0 : D1;
    wire unused = C1 | CE | R | S;
endmodule

module IOBUF (inout IO, input I, output O, input T);
    assign IO = T ? 1'bz : I;
    assign O = IO;
endmodule
