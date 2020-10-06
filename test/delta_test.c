#include <unity.h>
#include <stdlib.h>
#include "../src/delta.h"
#include "../src/util.h"


static cJSON* spec_located_at(float x, float y, float z, float sz)
{
  return cjson_create_object(
    "transform", cjson_create_object(
      "matrix", m2cjson(allo_m4x4_translate((allo_vector) {{ x, y, z }})),
      NULL
    ),
    NULL
  );
}

void test_matrices_equal(allo_m4x4 a, allo_m4x4 b)
{
  for (int i = 0; i < 16; i++) {
    TEST_ASSERT_DOUBLE_WITHIN_MESSAGE(
      0.001, a.v[i], b.v[i], "Index is wrong"
    );
  }
}

statehistory_t *sendhistory;
statehistory_t *recvhistory;
allo_state *state;
allo_entity *foo;
void setUp()
{
  sendhistory = calloc(1, sizeof(statehistory_t));
  recvhistory = calloc(1, sizeof(statehistory_t));
  state = calloc(1, sizeof(allo_state));
  allo_state_init(state);

  cJSON* root = spec_located_at(0, 0, 0, 0.3);
  foo = allo_state_add_entity_from_spec(state, NULL, root, NULL);
}
void tearDown()
{
  allo_delta_destroy(sendhistory);
  free(sendhistory);
  allo_delta_destroy(recvhistory);
  free(recvhistory);
  allo_state_destroy(state);
  free(state);
}

void test_basic(void)
{
  // initialize sender and receiver with an initial state
  cJSON *first = allo_state_to_json(state);
  allo_delta_insert(sendhistory, first);
  allo_delta_insert(recvhistory, first);

  // pretend we moved an object, insert that into sender's history....
  allo_m4x4 moved = allo_m4x4_translate((allo_vector){2, 3, 4});
  entity_set_transform(foo, moved);
  state->revision++;
  cJSON *second = allo_state_to_json(state);
  allo_delta_insert(sendhistory, second);

  // ... and then pretend-send it to receiver
  char *delta = allo_delta_compute(sendhistory, recvhistory->latest_revision);

  // ... and pretend-receive it
  cJSON *patch = cJSON_Parse(delta);
  cJSON *merged = allo_delta_apply(recvhistory, patch);
  TEST_ASSERT_NOT_NULL_MESSAGE(merged, "expected applying patch to succeed");
  free(delta);
  TEST_ASSERT_TRUE_MESSAGE(cJSON_Compare(second, merged, true), "expected patch to bring state up to speed");

  // and make sure the change is propagated to the actual entity rep
  cJSON *entsj = cJSON_GetObjectItem(merged, "entities");
  cJSON *fooj = cJSON_GetObjectItem(entsj, foo->id);
  cJSON *componentsj = cJSON_GetObjectItem(fooj, "components");
  cJSON *transformj = cJSON_GetObjectItem(componentsj, "transform");
  cJSON *matrix = cJSON_GetObjectItem(transformj, "matrix");
  allo_m4x4 moved2 = cjson2m(matrix);
  test_matrices_equal(moved, moved2);
}

int main(void)
{
  UNITY_BEGIN();

  RUN_TEST(test_basic);

  return UNITY_END();
}
