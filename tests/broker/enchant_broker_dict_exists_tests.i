#ifndef EnchantBrokerDictExistsTestFixture
#error EnchantBrokerDictExistsTestFixture must be defined as the testfixture class to run these tests against
#endif

#ifndef DictionaryExistsMethodCalled
#error DictionaryExistsMethodCalled must be defined as the variable that is set when the method is called
#endif

/**
 * enchant_broker_dict_exists
 * @broker: A non-null #EnchantBroker
 * @tag: The non-null language tag you wish to request a dictionary for ("en_US", "de_DE", ...)
 *
 * language tags are normalized by stripping leading and trailing whitespace,
 * removing '@' and any trailing characters, removing '.' and any trailing characters,
 * and replacing '-' with '_'
 *
 * If no providers have the given language tag, enchant will fallback to the language code
 * prefix (everything before the first "_")
 *
 * Return existance of the requested dictionary (1 == true, 0 == false)
 */

TEST_FIXTURE(EnchantBrokerDictExistsTestFixture, 
             EnchantBrokerDictExists_DictDoesNotExist_FalseOnlyAsksOnce)
{
  CHECK_EQUAL(0, enchant_broker_dict_exists(_broker, "en"));
  CHECK_EQUAL(1,DictionaryExistsMethodCalled);
}

TEST_FIXTURE(EnchantBrokerDictExistsTestFixture, 
             EnchantBrokerDictExists_DictExists_TrueAsksOnce)
{
  CHECK_EQUAL(1, enchant_broker_dict_exists(_broker, "en_GB"));
  CHECK_EQUAL(1,DictionaryExistsMethodCalled);
}

TEST_FIXTURE(EnchantBrokerDictExistsTestFixture, 
             EnchantBrokerDictExists_BaseDictExists_TrueAsksTwice)
{
  CHECK_EQUAL(1, enchant_broker_dict_exists(_broker, "qaa_CA"));
  CHECK_EQUAL(2,DictionaryExistsMethodCalled);
}

TEST_FIXTURE(EnchantBrokerDictExistsTestFixture, 
             EnchantBrokerDictExists_ExtraStuffAfterSecondUnderscore_NotStrippedForComparison)
{
  CHECK_EQUAL(0, enchant_broker_dict_exists(_broker, "en_GB_special"));
}

TEST_FIXTURE(EnchantBrokerDictExistsTestFixture, 
             EnchantBrokerDictExists_WhitespaceSurroundingLanguageTag_Removed)
{
  CHECK_EQUAL(1, enchant_broker_dict_exists(_broker, "\n\r en_GB \t\f"));
}

/* Vertical tab is not considered to be whitespace in glib!
    See bug# 59388 http://bugzilla.gnome.org/show_bug.cgi?id=59388
*/
TEST_FIXTURE(EnchantBrokerDictExistsTestFixture, 
             EnchantBrokerDictExists_VerticalTabSurroundingLanguageTag_NotRemoved)
{
  CHECK_EQUAL(0, enchant_broker_dict_exists(_broker, "\ven_GB"));
  CHECK_EQUAL(0, enchant_broker_dict_exists(_broker, "en_GB\v"));
}

TEST_FIXTURE(EnchantBrokerDictExistsTestFixture, 
             EnchantBrokerDictExists_AtSignInLanguageTag_RemovesToTail)
{
  CHECK_EQUAL(1, enchant_broker_dict_exists(_broker, "en_GB@euro"));
}

TEST_FIXTURE(EnchantBrokerDictExistsTestFixture, 
             EnchantBrokerDictExists_NothingLeftAfterNormalizingLanguageTag_DoesNotCallProvider)
{
  CHECK_EQUAL(0, enchant_broker_dict_exists(_broker, "@euro"));
  CHECK_EQUAL(0,DictionaryExistsMethodCalled);
}


TEST_FIXTURE(EnchantBrokerDictExistsTestFixture, 
             EnchantBrokerDictExists_PeriodInLanguageTag_RemovesToTail)
{
  CHECK_EQUAL(1, enchant_broker_dict_exists(_broker, "en_GB.UTF-8"));
}

TEST_FIXTURE(EnchantBrokerDictExistsTestFixture, 
             EnchantBrokerDictExists_HyphensInLanguageTag_SubstitutedWithUnderscore)
{
  CHECK_EQUAL(1, enchant_broker_dict_exists(_broker, "en-GB"));
}

TEST_FIXTURE(EnchantBrokerDictExistsTestFixture, 
             EnchantBrokerDictExists_CalledWhenDictionaryIsNotInUse_MakesCall)
{
  DictionaryExistsMethodCalled = false;
  enchant_broker_dict_exists(_broker, "en-GB");
  CHECK_EQUAL(1,DictionaryExistsMethodCalled);
}

TEST_FIXTURE(EnchantBrokerDictExistsTestFixture, 
             EnchantBrokerDictExists_DifferentCase_Finds)
{
  CHECK_EQUAL(1, enchant_broker_dict_exists(_broker, "EN_gb"));
}

TEST_FIXTURE(EnchantBrokerDictExistsTestFixture, 
             EnchantBrokerDictExists_DifferentCase_NoRegion_Finds)
{
  CHECK_EQUAL(1, enchant_broker_dict_exists(_broker, "QAA"));
}


TEST_FIXTURE(EnchantBrokerDictExistsTestFixture, 
             EnchantBrokerDictExists_HasPreviousError_ErrorCleared)
{
  SetErrorOnMockProvider("something bad happened");

  enchant_broker_dict_exists(_broker, "en_US");

  CHECK_EQUAL((void*)NULL, (void*)enchant_broker_get_error(_broker));
}

/////////////////////////////////////////////////////////////////////////////
// Test Error Conditions

TEST_FIXTURE(EnchantBrokerDictExistsTestFixture, 
			 EnchantBrokerDictExists_NullBroker_0)
{
    CHECK_EQUAL(0, enchant_broker_dict_exists (NULL, "en_US"));
}

TEST_FIXTURE(EnchantBrokerDictExistsTestFixture, 
             EnchantBrokerDictExists_NullTag_0)
{
    CHECK_EQUAL(0, enchant_broker_dict_exists (_broker, NULL));
}

TEST_FIXTURE(EnchantBrokerDictExistsTestFixture, 
             EnchantBrokerDictExists_EmptyTag_0)
{
    CHECK_EQUAL(0, enchant_broker_dict_exists (_broker, ""));
}

TEST_FIXTURE(EnchantBrokerDictExistsTestFixture, 
             EnchantBrokerDictExists_InvalidTag_0_ErrorSet)
{
    CHECK_EQUAL(0, enchant_broker_dict_exists (_broker, "en~US"));
    CHECK(NULL != enchant_broker_get_error(_broker));
}
