#file =open('obj-dump-interp-corei9.txt', 'r')
#data = file.read()
#print('data = ', data)

file = open('obj-dump-interp-corei9.txt', 'r')

searchOpcode = ['__stubACC0', '__stubACC1', '__stubACC2', '__stubACC3', '__stubACC4', '__stubAPPTERM3']

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
    
