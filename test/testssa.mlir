//===------------------------------------------------------------------------------------------===//
// Test module for the value semantics the QuantumSSA dialect
//===------------------------------------------------------------------------------------------===//

module {
    %fp = constant 1.0 : f64
    %size = constant 8 : index

    %q0_0 = qs.alloc -> !qs.qstate
    %r0_0 = qs.allocreg(4) -> !qs.rstate<4>
    %r1_0 = qs.allocreg(%size) : index -> !qs.rstate<>

    %h = qs.H -> !qs.u1
    %q0_1 = qs.H %q0_0 : !qs.qstate -> !qs.qstate
    %q0_2 = qs.H %q0_0 : !qs.qstate -> !qs.qstate       // state reuse is illegal but not enforced
    %r0_1 = qs.H %r0_0 : !qs.rstate<4> -> !qs.rstate<4>
    //qs.H %q0_0 : !qs.qstate                           // must return new state
    //qs.H %q0_0 : !qs.qstate -> !qs.u1                 // must return state not op
    //%q0_3 = qs.H %r0_1 : !qs.rstate<4> -> !qs.qstate  // cannot mix qubit & register states

    %q0_3 = qs.X %q0_2 : !qs.qstate -> !qs.qstate
    %q0_4 = qs.RZ(%fp) %q0_3 : f64, !qs.qstate -> !qs.qstate
    %q0_5 = qs.R(%fp) %q0_4 : f64, !qs.qstate -> !qs.qstate
    %q0_6 = qs.CX %q0_0, %q0_5 : !qs.qstate, !qs.qstate -> !qs.qstate
    %r0_2 = qs.CX %q0_0, %r0_1 : !qs.qstate, !qs.rstate<4> -> !qs.rstate<4>
    //%q0_6 = qs.CX %q0_5, %q0_5 : !qs.qstate, !qs.qstate -> !qs.qstate // same trgt & ctrl

    %ch = qs.c %h, %q0_0 : !qs.u1, !qs.qstate -> !qs.cop<1, !qs.u1>
    %q0_7 = qs.c %h, %q0_0, %q0_6 : !qs.u1, !qs.qstate, !qs.qstate -> !qs.qstate
    %q0_8 = qs.c %h, %r0_0, %q0_7 : !qs.u1, !qs.rstate<4>, !qs.qstate -> !qs.qstate
    %r0_3 = qs.c %h, %q0_0, %r0_2 : !qs.u1, !qs.qstate, !qs.rstate<4> -> !qs.rstate<4>

    %ah = qs.adj %h : !qs.u1 -> !qs.u1
    %q0_9 = qs.adj %h, %q0_8 : !qs.u1, !qs.qstate -> !qs.qstate
    %r0_4 = qs.adj %h, %r0_3 : !qs.u1, !qs.rstate<4> -> !qs.rstate<4>

    // test new return statement
    func @retTest(%arg : !qs.qstate) -> !qs.qstate {
        qs.return %arg : !qs.qstate
    }

    // test new function circuit type
    %circ = qs.funcirc @retTest -> !qs.fcirc

    // apply meta op the function circuit
    %ccirc = qs.c %circ, %q0_0 : !qs.fcirc, !qs.qstate -> !qs.cop<1, !qs.fcirc>
    %acirc = qs.adj %circ : !qs.fcirc -> !qs.fcirc

    // execute the function circuits
    %q0_10 = call @retTest(%q0_9) : (!qs.qstate) -> !qs.qstate
    //%q0_14 = qs.applyfc %ch(%q0_13) : !qs.cop<1, !qs.u1>> // only works on fcirc(-derived) types
    %q0_11 = qs.applyfc %circ(%q0_10) : !qs.fcirc, !qs.qstate -> !qs.qstate
    %q0_12 = qs.applyfc %ccirc(%q0_11) : !qs.cop<1, !qs.fcirc>, !qs.qstate -> !qs.qstate
    %q0_13 = qs.applyfc %acirc(%q0_12) : !qs.fcirc, !qs.qstate -> !qs.qstate
}
