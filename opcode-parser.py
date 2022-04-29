#file =open('obj-dump-interp-corei9.txt', 'r')
#data = file.read()
#print('data = ', data)

file = open('obj-dump-interp-corei9.txt', 'r')

searchOpcode = ['<__stubACC0>:', '<__stubACC>:', '<__stubACC2>:', '<__stubACC3>:', '<__stubACC4>:', '<__stubACC5>:', '<__stubACC6>:', '<__stubACC7>:',
        '<__stubACC>:', '<__stubPUSH>:', '<__stubPUSHACC0>:', '<__stubPUSHACC1>:', '<__stubPUSHACC2>:', '<__stubPUSHACC3>:', '<__stubPUSHACC4>:', 
        '<__stubPUSHACC5>:', '<__stubPUSHACC6>:', '<__stubPUSHACC7>:', '<__stubPUSHACC>:', '<__stubPOP>:', '<__stubASSIGN>:', '<__stubENVACC1>:', '<__stubENVACC2>:', 
        '<__stubENVACC3>:', '<__stubENVACC4>:', '<__stubENVACC>:', '<__stubPUSHENVACC1>:', '<__stubPUSHENVACC2>:', '<__stubPUSHENVACC3>:', '<__stubPUSHENVACC4>:',
        '<__stubPUSHENVACC>:', '<__stubPUSH_RETADDR>:', '<__stubAPPLY>:', '<__stubAPPLY1>:', '<__stubAPPLY2>:', '<__stubAPPLY3>:', '<__stubAPPTERM>:', '<__stubAPPTERM1>:',
        '<__stubAPPTERM2>:', '<__stubAPPTERM3>:', '<__stubRETURN>:', '<__stubGRAB>:', '<__stubCLOSURE>:', '<__stubCLOSUREREC>:', '<__stubOFFSETCLOSUREM3>:', 
        '<__stubOFFSETCLOSURE0>:', '<__stubOFFSETCLOSURE3>:', '<__stubOFFSETCLOSURE>:', '<__stubPUSHOFFSETCLOSUREM3>:', '<__stubPUSHOFFSETCLOSURE0>:', 
        '<__stubPUSHOFFSETCLOSURE3>:', '<__stubOFFSETCLOSURE>:', '<__stubGETGLOBAL>:', '<__stubPUSHGETGLOBAL>:', '<__stubGETGLOBALFIELD>:', 
        '<__stubPUSHGETGLOBALFIELD>:', '<__stubSETGLOBAL>:', '<__stubATOM0>:', '<__stubATOM>:', '<__stubPUSHATOM0>:', '<__stubPUSHATOM>:','<__stubMAKEBLOCK>:', 
        '<__stubMAKEBLOCK1>:', '<__stubMAKEBLOCK2>:', '<__stubMAKEBLOCK3>:', '<__stubMAKEFLOATBLOCK>:', '<__stubGETFIELD0>:', '<__stubGETFIELD1>:', '<__stubGETFIELD2>:',
        '<__stubGETFIELD3>:', '<__stubGETFIELD>:', '<__stubGETFLOATFIELD>:', '<__stubSETFIELD0>:', '<__stubSETFIELD1>:', '<__stubSETFIELD2>:', '<__stubSETFIELD3>:', 
        '<__stubSETFIELD>:', '<__stubSETFLOATFIELD>:', '<__stubVECTLENGTH>:', '<__stubGETVECTITEM>:', '<__stubSETVECTITEM>:', '<__stubGETBYTESCHAR>:', 
        '<__stubSETBYTESCHAR>:', '<__stubBRANCH>:', '<__stubBRANCHIF>:', '<__stubBRANCHIFNOT>:', '<__stubSWITCH>:', '<__stubBOOLNOT>:', '<__stubPUSHTRAP>:', 
        '<__stubPOPTRAP>:', '<__stubRAISE>:', '<__stubCHECK_SIGNALS>:', '<__stubC_CALL1>:', '<__stubC_CALL2>:', '<__stubC_CALL3>:', '<__stubC_CALL4>:',
        '<__stubC_CALL5>:', '<__stubC_CALLN>:', '<__stubCONST0>:', '<__stubCONST1>:', '<__stubCONST2>:', '<__stubCONST3>:', '<__stubCONSTINT>:', '<__stubPUSHCONST0>:', 
        '<__stubPUSHCONST1>:', '<__stubPUSHCONST2>:', '<__stubPUSHCONST3>:', '<__stubPUSHCONSTINT>:', '<__stubNEGINT>:', '<__stubADDINT>:', '<__stubSUBINT>:', 
        '<__stubMULINT>:', '<__stubDIVINT>:', '<__stubMODINT>:', '<__stubANDINT>:', '<__stubORINT>:', '<__stubXORINT>:', '<__stubLSLINT>:', '<__stubLSRINT>:',
        '<__stubASRINT>:', '<__stubEQ>:', '<__stubNEQ>:', '<__stubLTINT>:', '<__stubLEINT>:', '<__stubGTINT>:', '<__stubGEINT>:', '<__stubOFFSETINT>:', 
        '<__stubOFFSETREF>:', '<__stubISINT>:', '<__stubGETMETHOD>:', '<__stubBEQ>:', '<__stubBNEQ>:', '<__stubBLTINT>:', '<__stubBLEINT>:', '<__stubBGTINT>:', 
        '<__stubBGEINT>:', '<__stubULTINT>:', '<__stubUGEINT>:', '<__stubUGEINT>:', '<__stubBULTINT>:', '<__stubBUGEINT>:', '<__stubGETPUBMET>:', '<__stubGETDYNMET>:',
        '<__stubSTOP>:', '<__stubEVENT>:', '<__stubBREAK>:', '<__stubRERAISE>:', '<__stubRAISE_NOTRACE>:', '<__stubGETSTRINGCHAR>:', '<__stubPERFORM>:', 
        '<__stubRESUME>:', '<__stubRESUMETERM>:', '<__stubREPERFORMTERM>:', '<__stubFIRST_UNIMPLEMENTED_OP>:'] 

for line in file:
    if any(word in line for word in searchOpcode):
        print(line)
#with open('obj-dump-interp-corei9.txt', 'r') as file:
#    for l_no, line in enumerate(file):
        # search string
#        if searchOpcode in line:
#            print('string found in a file')
#            print('Line number:', l_no)
#            print('Line:', line) 
            # This will not look at other lines 
            # break
        
#what2Return = "None" 
#if str2Find in file.read():
#    what2Return = str2Find
#    print('found = ', what2Return)
    
