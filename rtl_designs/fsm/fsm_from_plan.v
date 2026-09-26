module fsm(IN, CLK, RST, MATCH);
input IN;
input CLK;
input RST;
output reg MATCH;
parameter s0 = 3'd0;
parameter s1 = 3'd1;
parameter s2 = 3'd2;
parameter s3 = 3'd3;
parameter s4 = 3'd4;
reg [2:0] ST_cr, ST_nt;
always @(posedge CLK or posedge RST) begin
  if (RST) ST_cr <= s0;
  else ST_cr <= ST_nt;
end
always @(*) begin
  case (ST_cr)
    s0: if (IN == 1'b0) ST_nt = s0; else ST_nt = s1;
    s1: if (IN == 1'b0) ST_nt = s2; else ST_nt = s1;
    s2: if (IN == 1'b0) ST_nt = s3; else ST_nt = s1;
    s3: if (IN == 1'b0) ST_nt = s0; else ST_nt = s4;
    s4: if (IN == 1'b0) ST_nt = s2; else ST_nt = s1;
    default: ST_nt = s0;
  endcase
end
always @(*) begin
  if (RST) MATCH = 1'b0;
  else if (ST_cr == s4 && IN == 1'b1) MATCH = 1'b1;
  else MATCH = 1'b0;
end
endmodule
