from CleosOutputVerification import *

import unittest
import hashlib
import json
import re

def verifyContractHash(output, contractName, contractsManager):
    path = contractsManager.getWasmFilePath(contractName)
    file = open(path, 'rb')
    hash = hashlib.sha256()
    hash.update(file.read())
    file.close()
    verifyOutputContains(output, hash.hexdigest())

def verifyContractAbi(output, contractName, contractsManager):
    path = contractsManager.getAbiFilePath(contractName)
    file = open(path, 'r')
    abiObjectFromFile = json.loads(file.read())
    file.close()
    abiObjectFromNode = json.loads("".join(output))

    # as abi file serialized/desiralizes on node some fields are absent in the source file.
    # check abi files by key fields
    unittest.TestCase.assertEqual(unittest.TestCase(), abiObjectFromNode['version'], abiObjectFromFile['version'])
    unittest.TestCase.assertEqual(unittest.TestCase(), abiObjectFromNode['structs'], abiObjectFromFile['structs'])

    for sourceTable in abiObjectFromFile['actions']:
        for testTable in abiObjectFromNode['actions']:
            if sourceTable['name'] == testTable['name']:
                unittest.TestCase.assertEqual(unittest.TestCase(), sourceTable['type'], testTable['type'])


    for sourceTable in abiObjectFromFile['tables']:
        for testTable in abiObjectFromNode['tables']:
            if sourceTable['name'] == testTable['name']:
                unittest.TestCase.assertEqual(unittest.TestCase(), sourceTable['type'], testTable['type'])

def verifyGetNewAccount(output, testKey):
    verifyOutputContainsRegex(output, "[ ]+owner[ ]+1:[ ]+1 " + testKey)
    verifyOutputContainsRegex(output, "[ ]+active[ ]+1:[ ]+1 " + testKey)
    verifyAccountLiquidBalance(output, "0.0000 CYBER")
    verifyAccountStakeBalance(output, "0.0000 CYBER")
    verifyAccountReceivedBalance(output, "0.0000 CYBER")
    verifyAccountProvidedBalance(output, "0.0000 CYBER")
    verifyAccountUnstakingBalance(output, "0.0000 CYBER")
    verifyAccountOwnedBalance(output, "0.0000 CYBER")
    verifyAccountEffectiveBalance(output, "0.0000 CYBER")
    verifyAccountTotalBalance(output, "0.0000 CYBER")

def verifyAccountLiquidBalance(output, amount):
    verifyOutputContainsRegex(output, "[ ]+liquid:[ ]+" + amount)

def verifyAccountStakeBalance(output, amount):
    verifyOutputContainsRegex(output, "[ ]+staked:[ ]+" + amount)

def verifyAccountEffectiveBalance(output, amount):
    verifyOutputContainsRegex(output, "[ ]+effective:[ ]+" + amount)

def verifyAccountUnstakingBalance(output, amount):
    verifyOutputContainsRegex(output, "[ ]+unstaking:[ ]+" + amount)

def verifyAccountOwnedBalance(output, amount):
    verifyOutputContainsRegex(output, "[ ]+owned:[ ]+" + amount)

def verifyAccountReceivedBalance(output, amount):
    verifyOutputContainsRegex(output, "[ ]+recieved:[ ]+" + amount)

def verifyAccountProvidedBalance(output, amount):
    verifyOutputContainsRegex(output, "[ ]+provided:[ ]+" + amount)

def verifyAccountTotalBalance(output, amount):
    verifyOutputContainsRegex(output, "[ ]+total:[ ]+" + amount)

def verifyStakeCreated(output, symbol):
    verifyOutputContainsRegex(output, "#[ ]+cyber.stake <= cyber.stake::create[ ]+{\"token_symbol\":\"" + symbol)

def verifyResoursesStakeEndowment(output):
    effective = getEffectiveBalance(output)

    memoryStaked = getResourceStaked(output, "memory")
    storageStaked = getResourceStaked(output, "storage")
    netStaked = getResourceStaked(output, "net bandwidth")
    cpuStaked = getResourceStaked(output, "cpu bandwidth")

    received = getReceivedBalance(output)

    memoryReceived = getResourceReceived(output, "memory")
    storageReceived = getResourceReceived(output, "storage")
    netReceived = getResourceReceived(output, "net bandwidth")
    cpuReceived = getResourceReceived(output, "cpu bandwidth")

    resourcesReceived = memoryReceived + storageReceived + netReceived + cpuReceived
    resourcesStaked = memoryStaked + storageStaked + netStaked + cpuStaked
    resourcesEffective = resourcesStaked + resourcesReceived

    unittest.TestCase.assertTrue(unittest.TestCase(), effective - resourcesEffective <= 1)
    unittest.TestCase.assertTrue(unittest.TestCase(), received - resourcesReceived <= 1)

def getEffectiveBalance(output):
    return getBalance(output, "effective")

def getReceivedBalance(output):
    return getBalance(output, "recieved")

def getBalance(output, type):
    balances = False
    for line in output:
        if balances == False and re.match("CYBER[ ]+balances:", line):
            balances = True

        if balances == True and re.match("[ ]+" + type + ":[ ]+[0-9.]+ CYBER", line) :
            return getBalanceValue(line)

def getResourceStaked(output, resource):
    return getResourceCost(output, resource, "staked")

def getResourceReceived(output, resource):
    return getResourceCost(output, resource, "delegated")

def getResourceCost(output, resource, type):
    resourceFound = False
    for line in output:
        if resourceFound == False and re.match(resource + ":", line):
            resourceFound = True

        if resourceFound == True and re.match("[ ]+" + type + ":[ ]+[0-9.]+ CYBER", line) :
            return getBalanceValue(line)

def getBalanceValue(string):
    value = re.findall("[0-9.]+", string)[0]

    return int(re.sub('\.', '', value))

def verifyResoursesStakeUsage(output):
    #implement stake usage calculation in the api
    return
