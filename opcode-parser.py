#file =open('obj-dump-interp-corei9.txt', 'r')
#data = file.read()
#print('data = ', data)

with open('obj-dump-interp-corei9.txt', 'r') as file:
    for l_no, line in enumerate(file):
        # search string
        if 'ACC0' in line:
            print('string found in a file')
            print('Line number:', l_no)
            print('Line:', line) 
            # This will not look at other lines 
            # break
        
#what2Return = "None" 
#if str2Find in file.read():
#    what2Return = str2Find
#    print('found = ', what2Return)
    
